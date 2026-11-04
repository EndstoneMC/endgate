#include "endgate/network/endpoint.h"

#ifndef _WIN32
#include <ifaddrs.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/cancel_after.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>
#ifdef _WIN32
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "endgate/auth/token.h"
#include "endgate/awaitable.h"
#include "endgate/connection/handshake.h"
#include "endgate/connection/nethernet.h"
#include "endgate/connection/player.h"
#include "endgate/connection/raknet.h"
#include "endgate/logging/manager.h"
#include "endgate/network/nethernet/candidates.h"
#include "endgate/network/query.h"
#include "endgate/network/rtc/stun.h"
#include "endgate/server.h"

namespace endgate {

namespace {

std::optional<asio::ip::address> toInterfaceAddress(const sockaddr *address, bool with_v6)
{
    const auto family = address == nullptr ? AF_UNSPEC : address->sa_family;
    if (family != AF_INET && (family != AF_INET6 || !with_v6)) {
        return std::nullopt;
    }
    asio::ip::udp::endpoint endpoint;
    const auto size = family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    std::memcpy(endpoint.data(), address, size);
    endpoint.resize(size);
    return endpoint.address();
}

std::vector<asio::ip::address> getInterfaceAddresses(bool with_v6)
{
    std::vector<asio::ip::address> addresses;
#ifdef _WIN32
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    std::vector<std::byte> storage;
    ULONG size = 0;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    while (result == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        result = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                        reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()), &size);
    }
    if (result != NO_ERROR) {
        return addresses;
    }
    for (const auto *adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES *>(storage.data()); adapter != nullptr;
         adapter = adapter->Next) {
        for (const auto *unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            if (const auto address = toInterfaceAddress(unicast->Address.lpSockaddr, with_v6)) {
                addresses.push_back(*address);
            }
        }
    }
#else
    ifaddrs *list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return addresses;
    }
    const std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> owned(list, ::freeifaddrs);
    for (const auto *entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (const auto address = toInterfaceAddress(entry->ifa_addr, with_v6)) {
            addresses.push_back(*address);
        }
    }
#endif
    return addresses;
}

}  // namespace

Endpoint::Endpoint(ProxyServer &server, EventLoop &event_loop, Registry &registry,
                   std::shared_ptr<asio::ip::udp::socket> socket, std::uint64_t guid,
                   std::span<const std::byte> offline_ping_response,
                   std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> identity,
                   std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> offer_identity,
                   std::shared_ptr<asio::ssl::context> tls)
    : server_(server), logger_(LogManager::getLogger(*this)), event_loop_(event_loop), registry_(registry),
      socket_(std::move(socket)), offline_(*this, guid), certificate_(rtc::Certificate::shared()),
      identity_(std::move(identity)), offer_identity_(std::move(offer_identity)),
      session_config_{.max_packet_size = 8 * 1024 * 1024},
      signaling_(nethernet::HttpSignalingServer::create(event_loop, *this, {}, std::move(tls)))
{
    offline_.setOfflinePingResponse(Unpooled::wrappedBuffer(offline_ping_response));
}

Endpoint::~Endpoint() = default;

std::string Endpoint::getLocalAddress() const
{
    return toString(socket_->local_endpoint());
}

void Endpoint::listen(const asio::ip::tcp::endpoint &endpoint)
{
    signaling_->listen(endpoint);
}

void Endpoint::start()
{
    signaling_->start();
    asio::co_spawn(event_loop_.getExecutor(), receive(), asio::detached);
}

Awaitable<void> Endpoint::receive()
{
    std::array<std::byte, 65536> buffer{};
    asio::ip::udp::endpoint sender;
    for (;;) {
        const auto [ec, size] =
            co_await socket_->async_receive_from(asio::buffer(buffer), sender, asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted || closed_) {
            co_return;
        }
        if (ec || size == 0) {
            continue;
        }
        handleDatagram(std::span{buffer}.first(size), sender);
    }
}

void Endpoint::sendTo(const raknet::SystemAddress &address, ByteBufView datagram)
{
    asio::error_code ignored;
    socket_->send_to(asio::buffer(datagram.nioBuffer()), address, 0, ignored);
}

void Endpoint::handleDatagram(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender)
{
    constexpr std::size_t forward_limit = 4096;

    if (query_ != nullptr && GameSpyQueryHandler::isQuery(datagram) && !routes_.contains(sender)) {
        query_->channelRead0(datagram, sender, *socket_);
        return;
    }

    const auto buffer = Unpooled::wrappedBuffer(datagram);
    const auto id = static_cast<raknet::MessageId>(datagram.front());
    const bool offline_message = raknet::isOfflineMessage(buffer.slice());
    if (offline_message &&
        (id == raknet::MessageId::UnconnectedPing || id == raknet::MessageId::UnconnectedPingOpenConnections ||
         id == raknet::MessageId::OpenConnectionRequest1)) {
        static_cast<void>(offline_.handle(buffer.slice(), sender, true));
        return;
    }
    Endpoint *owner = nullptr;
    if (const auto peeked = rtc::StunMessage::peekLocalUfrag(buffer.slice())) {
        const std::string ufrag(*peeked);
        if (const auto session = ufrags_.find(ufrag); session != ufrags_.end()) {
            const auto connection = session->second;
            connection->handleDatagram(datagram, sender);
            return;
        }
        const auto check = rtc::StunMessage::decode(buffer.slice());
        if (!check || closed_) {
            return;
        }
        auto claim = registry_.claim(ufrag, *this, *check, buffer.slice());
        if (auto *const join = std::get_if<PendingJoin>(&claim)) {
            const auto connection = std::make_shared<NetherNetConnection<nethernet::NetworkSession>>(
                *this, join->network_id, join->offer, std::move(join->credentials), certificate_, session_config_,
                join->created);
            connection->setSessionHandler(
                std::make_shared<HandshakeSessionHandler>(server_, connection, std::move(join->identity)));
            connections_.emplace(connection, Registration{.ufrag = ufrag});
            ufrags_.emplace(ufrag, connection);
            forwards_.erase(sender);
            connection->handleDatagram(datagram, sender);
            return;
        }
        if (const auto *const claimed = std::get_if<Endpoint *>(&claim)) {
            owner = *claimed;
            if (forwards_.insert_or_assign(sender, owner).second) {
                forward_order_.push_back(sender);
                while (forward_order_.size() > forward_limit) {
                    forwards_.erase(forward_order_.front());
                    forward_order_.pop_front();
                }
            }
        }
    }
    else if (const auto route = routes_.find(sender); route != routes_.end()) {
        const auto connection = route->second;
        connection->handleDatagram(datagram, sender);
        return;
    }
    else if (offline_message) {
        if (id == raknet::MessageId::OpenConnectionRequest2) {
            accept(datagram, sender);
        }
        return;
    }
    else if (const auto cached = forwards_.find(sender); cached != forwards_.end()) {
        owner = cached->second;
    }
    if (owner != nullptr && owner != this) {
        owner->getEventLoop().execute([owner, copy = Unpooled::copiedBuffer(datagram), sender, origin = this] {
            owner->forward(copy, sender, *origin);
        });
    }
}

void Endpoint::forward(const ByteBuf &datagram, const asio::ip::udp::endpoint &sender, Endpoint &origin)
{
    if (!closed_ && datagram.isReadable()) {
        if (const auto peeked = rtc::StunMessage::peekLocalUfrag(Unpooled::wrappedBuffer(datagram.nioBuffer()))) {
            if (const auto session = ufrags_.find(std::string(*peeked)); session != ufrags_.end()) {
                const auto connection = session->second;
                connection->handleDatagram(datagram.nioBuffer(), sender);
                return;
            }
        }
        else if (const auto route = routes_.find(sender); route != routes_.end()) {
            const auto connection = route->second;
            connection->handleDatagram(datagram.nioBuffer(), sender);
            return;
        }
    }
    origin.getEventLoop().execute([&origin, owner = this, sender] { origin.invalidate(sender, owner); });
}

void Endpoint::invalidate(const asio::ip::udp::endpoint &sender, Endpoint *owner)
{
    if (const auto cached = forwards_.find(sender); cached != forwards_.end() && cached->second == owner) {
        forwards_.erase(cached);
    }
}

void Endpoint::accept(std::span<const std::byte> datagram, const raknet::SystemAddress &sender)
{
    const auto request = offline_.handle(Unpooled::wrappedBuffer(datagram), sender, true);
    if (!request) {
        return;
    }
    const auto guid = request->client_guid == raknet::UNASSIGNED_RAKNET_GUID
                        ? std::nullopt
                        : std::optional<std::uint64_t>{request->client_guid};
    if (guid && !registry_.addGuid(*guid)) {
        offline_.reject(sender, raknet::MessageId::AlreadyConnected);
        return;
    }
    if (!registry_.addHalfOpen()) {
        if (!half_open_.empty()) {
            const auto oldest = half_open_.begin()->second;
            half_open_.erase(half_open_.begin());
            if (const auto registration = connections_.find(oldest); registration != connections_.end()) {
                registration->second.half_open.reset();
                registry_.removeHalfOpen();
                unroute(oldest, registration->second);
            }
            oldest->shutdown();
        }
        if (!registry_.addHalfOpen()) {
            if (guid) {
                registry_.removeGuid(*guid);
            }
            return;
        }
    }

    const auto system_index = getFreeSystemIndex();
    const auto connection = std::make_shared<RakNetConnection>(*this, sender, *request, offline_.getGuid(),
                                                               system_index, raknet::Clock::now());
    connection->setSessionHandler(std::make_shared<HandshakeSessionHandler>(server_, connection, std::nullopt));
    const auto half_open = next_half_open_++;
    connections_.emplace(
        connection,
        Registration{.routes = {sender}, .guid = guid, .system_index = system_index, .half_open = half_open});
    half_open_.emplace(half_open, connection);
    routes_.insert_or_assign(sender, connection);
    connection->handleDatagram(datagram, sender);
}

std::uint16_t Endpoint::getFreeSystemIndex() const
{
    std::vector<bool> used(connections_.size() + 1);
    for (const auto &[connection, registration] : connections_) {
        if (registration.system_index && *registration.system_index < used.size()) {
            used[*registration.system_index] = true;
        }
    }
    return static_cast<std::uint16_t>(std::ranges::find(used, false) - used.begin());
}

void Endpoint::unroute(const std::shared_ptr<MinecraftConnection> &connection, Registration &registration)
{
    for (const auto &remote : std::exchange(registration.routes, {})) {
        if (const auto route = routes_.find(remote); route != routes_.end() && route->second == connection) {
            routes_.erase(route);
        }
    }
}

void Endpoint::registerConnection(const std::shared_ptr<MinecraftConnection> &connection,
                                  std::optional<std::uint64_t> guid)
{
    if (closed_) {
        connection->shutdown();
        return;
    }
    if (guid && !registry_.addGuid(*guid)) {
        guid.reset();
    }
    connections_.emplace(connection, Registration{.guid = guid});
}

void Endpoint::unregisterConnection(const std::shared_ptr<MinecraftConnection> &connection)
{
    const auto registration = connections_.find(connection);
    if (registration == connections_.end()) {
        return;
    }
    unroute(connection, registration->second);
    if (registration->second.guid) {
        registry_.removeGuid(*registration->second.guid);
    }
    if (registration->second.half_open) {
        half_open_.erase(*registration->second.half_open);
        registry_.removeHalfOpen();
    }
    if (const auto session = ufrags_.find(registration->second.ufrag);
        session != ufrags_.end() && session->second == connection) {
        ufrags_.erase(session);
        registry_.release(registration->second.ufrag);
    }
    connections_.erase(registration);
}

void Endpoint::activated(const std::shared_ptr<MinecraftConnection> &connection)
{
    const auto registration = connections_.find(connection);
    if (registration == connections_.end()) {
        return;
    }
    if (const auto half_open = std::exchange(registration->second.half_open, std::nullopt)) {
        half_open_.erase(*half_open);
        registry_.removeHalfOpen();
    }
}

void Endpoint::addRoute(const asio::ip::udp::endpoint &remote, const std::shared_ptr<MinecraftConnection> &connection)
{
    const auto registration = connections_.find(connection);
    if (registration == connections_.end()) {
        return;
    }
    if (!std::ranges::contains(registration->second.routes, remote)) {
        registration->second.routes.push_back(remote);
    }
    routes_.insert_or_assign(remote, connection);
}

void Endpoint::removeRoute(const asio::ip::udp::endpoint &remote,
                           const std::shared_ptr<MinecraftConnection> &connection)
{
    const auto registration = connections_.find(connection);
    if (registration == connections_.end()) {
        return;
    }
    std::erase(registration->second.routes, remote);
    if (const auto route = routes_.find(remote); route != routes_.end() && route->second == connection) {
        routes_.erase(route);
    }
}

ServerPing Endpoint::getServerPing(const nethernet::PingRequest &request)
{
    return server_.getServerListPingHandler().getInitialPing(request.protocol);
}

void Endpoint::onJoin(nethernet::JoinRequest request, nethernet::JoinResponse response)
{
    asio::co_spawn(event_loop_.getExecutor(), join(std::move(request), std::move(response)), asio::detached);
}

Awaitable<void> Endpoint::join(nethernet::JoinRequest request, nethernet::JoinResponse response)
{
    constexpr std::chrono::seconds resolve_timeout{2};

    const auto token = nethernet::ClientIdentityAssertionVerifier::verify(request.offer);
    if (!token) {
        logger_.debug("Refusing NetherNet network {}: its identity doesn't verify", request.network_id);
        response.reject(static_cast<std::uint32_t>(nethernet::ESessionError::IdentityNotAllowed));
        co_return;
    }
    std::optional<GameServerToken> identity;
    if (auto *const keys = server_.getMinecraftServiceKeyManager()) {
        auto validated = GameServerToken::validate(*token, *keys);
        if (!validated) {
            logger_.debug("Refusing NetherNet network {}: its token fails validation ({})", request.network_id,
                          bp::enum_name(validated.error()));
            response.reject(static_cast<std::uint32_t>(nethernet::ESessionError::IdentityNotAllowed));
            co_return;
        }
        identity = std::move(*validated);
    }
    const auto offer = rtc::Description::parse(nethernet::IdentityAssertion::stripFromSdp(request.offer));
    if (!offer) {
        response.reject(static_cast<std::uint32_t>(nethernet::ESessionError::FailedToSetRemoteDescription));
        co_return;
    }

    std::vector<asio::ip::address> reached;
    asio::error_code ec;
    if (const auto literal = asio::ip::make_address(request.host, ec); !ec) {
        reached.push_back(literal);
    }
    else if (auto cached = dns_.tryGet(request.host, MinecraftConnection::Clock::now())) {
        reached = std::move(*cached);
    }
    else if (!request.host.empty()) {
        asio::ip::udp::resolver resolver(event_loop_.getExecutor());
        const auto [error, results] = co_await resolver.async_resolve(
            request.host, "", asio::cancel_after(resolve_timeout, asio::as_tuple(asio::use_awaitable)));
        for (const auto &result : results) {
            reached.push_back(result.endpoint().address());
        }
        if (!error) {
            dns_.add(request.host, reached, MinecraftConnection::Clock::now());
        }
    }
    if (closed_) {
        response.reject(static_cast<std::uint32_t>(nethernet::ESessionError::GenericFailure));
        co_return;
    }

    const auto local = socket_->local_endpoint();
    std::vector<asio::ip::address> interfaces;
    if (!local.address().is_unspecified()) {
        interfaces.push_back(local.address());
    }
    else {
        interfaces = getInterfaceAddresses(local.address().is_v6());
    }

    const auto port = local.port();
    auto candidates = nethernet::gatherCandidates(reached, request.port.value_or(port), interfaces, port);
    const auto created = MinecraftConnection::Clock::now();
    auto credentials = rtc::IceCredentials::generate();
    auto added = Registry::AddPending::Collision;
    while ((added = registry_.addPending(PendingJoin{.offer = *offer,
                                                     .credentials = credentials,
                                                     .network_id = request.network_id,
                                                     .created = created,
                                                     .identity = identity})) == Registry::AddPending::Collision) {
        credentials = rtc::IceCredentials::generate();
    }
    if (added == Registry::AddPending::Full) {
        response.reject(static_cast<std::uint32_t>(nethernet::ESessionError::GenericFailure));
        co_return;
    }

    auto sdp = rtc::Description::answer(*offer, credentials, certificate_->getFingerprint(), candidates)
                   .generate(registry_.nextSessionId());
    if (identity_) {
        const auto assertion = nethernet::IdentityAssertion::create(*identity_, sdp, std::chrono::system_clock::now());
        if (!assertion) {
            registry_.removePending(credentials.ufrag);
            response.reject(static_cast<std::uint32_t>(nethernet::ESessionError::FailedToCreateIdentityAssertion));
            co_return;
        }
        sdp = assertion->injectIntoSdp(sdp);
    }
    expiring_.insert_or_assign(credentials.ufrag, event_loop_.schedule(
                                                      [this, ufrag = credentials.ufrag] {
                                                          registry_.removePending(ufrag);
                                                          expiring_.erase(ufrag);
                                                      },
                                                      session_config_.ice.negotiation_timeout));
    response.answer(std::move(sdp));
}

void Endpoint::close()
{
    closed_ = true;
    asio::error_code ignored;
    std::ignore = socket_->cancel(ignored);
    signaling_->close();
    for (const auto &[ufrag, expiry] : std::exchange(expiring_, {})) {
        expiry->cancel();
        registry_.removePending(ufrag);
    }
    for (const auto &[connection, registration] : std::exchange(connections_, {})) {
        if (registration.guid) {
            registry_.removeGuid(*registration.guid);
        }
        if (registration.half_open) {
            registry_.removeHalfOpen();
        }
        if (!registration.ufrag.empty()) {
            registry_.release(registration.ufrag);
        }
        connection->shutdown();
    }
    routes_.clear();
    ufrags_.clear();
    forwards_.clear();
    forward_order_.clear();
    half_open_.clear();
}

}  // namespace endgate
