#include "endgate/network/endpoint.h"

#include <ifaddrs.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <tuple>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/cancel_after.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>
#include <netinet/in.h>
#include <sys/socket.h>

#include "endgate/connection/nethernet.h"
#include "endgate/connection/player.h"
#include "endgate/connection/raknet.h"
#include "endgate/logging/manager.h"
#include "endgate/network/nethernet/candidates.h"
#include "endgate/network/rtc/stun.h"
#include "endgate/server.h"

namespace endgate {

Endpoint::Endpoint(ProxyServer &server, asio::io_context &io_context, std::shared_ptr<asio::ip::udp::socket> socket,
                   std::uint64_t guid, std::span<const std::byte> offline_ping_response,
                   std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> identity,
                   std::shared_ptr<asio::ssl::context> tls)
    : server_(server), logger_(LogManager::getLogger()), io_context_(io_context), socket_(std::move(socket)),
      strand_(asio::make_strand(io_context)), offline_(*this, guid), certificate_(rtc::Certificate::shared()),
      identity_(std::move(identity)), session_config_{.max_packet_size = 8 * 1024 * 1024},
      signaling_(nethernet::HttpSignalingServer::create(io_context, *this, {}, std::move(tls)))
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
    asio::co_spawn(strand_, receive(), asio::detached);
}

asio::awaitable<void> Endpoint::receive()
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
    const auto id = static_cast<raknet::MessageId>(datagram.front());
    const bool offline_message = raknet::isOfflineMessage(Unpooled::wrappedBuffer(datagram));
    if (offline_message &&
        (id == raknet::MessageId::UnconnectedPing || id == raknet::MessageId::UnconnectedPingOpenConnections ||
         id == raknet::MessageId::OpenConnectionRequest1)) {
        static_cast<void>(offline_.handle(Unpooled::wrappedBuffer(datagram), sender, true));
        return;
    }
    if (const auto route = routes_.find(sender); route != routes_.end()) {
        route->second->receive(datagram, sender);
        return;
    }
    if (const auto ufrag = rtc::StunMessage::peekLocalUfrag(datagram)) {
        if (const auto session = ufrags_.find(std::string(*ufrag)); session != ufrags_.end()) {
            session->second->receive(datagram, sender);
        }
        return;
    }
    if (offline_message && id == raknet::MessageId::OpenConnectionRequest2) {
        accept(datagram, sender);
    }
}

void Endpoint::accept(std::span<const std::byte> datagram, const raknet::SystemAddress &sender)
{
    constexpr std::size_t half_open_limit = 1024;

    const auto request = offline_.handle(Unpooled::wrappedBuffer(datagram), sender, true);
    if (!request) {
        return;
    }
    const auto guid = request->client_guid == raknet::UNASSIGNED_RAKNET_GUID
                        ? std::nullopt
                        : std::optional<std::uint64_t>{request->client_guid};
    if (guid && guids_.contains(*guid)) {
        offline_.reject(sender, raknet::MessageId::AlreadyConnected);
        return;
    }
    if (half_open_.size() >= half_open_limit) {
        const auto oldest = half_open_.begin()->second;
        half_open_.erase(half_open_.begin());
        if (const auto registration = connections_.find(oldest); registration != connections_.end()) {
            registration->second.half_open.reset();
            unroute(oldest, registration->second);
        }
        oldest->shutdown();
    }

    const auto system_index = getFreeSystemIndex();
    const auto connection = std::make_shared<RakNetConnection>(*this, sender, *request, offline_.getGuid(),
                                                               system_index, raknet::Clock::now());
    connection->setSessionHandler(
        std::make_shared<ClientPlaySessionHandler>(std::make_shared<Player>(server_, connection)));
    const auto half_open = next_half_open_++;
    connections_.emplace(
        connection,
        Registration{.routes = {sender}, .guid = guid, .system_index = system_index, .half_open = half_open});
    if (guid) {
        guids_.insert(*guid);
    }
    half_open_.emplace(half_open, connection);
    routes_.insert_or_assign(sender, connection);
    connection->receive(datagram, sender);
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

void Endpoint::registerConnection(std::shared_ptr<MinecraftConnection> connection, std::optional<std::uint64_t> guid)
{
    asio::post(strand_, [this, connection = std::move(connection), guid] {
        if (closed_) {
            connection->shutdown();
            return;
        }
        connections_.emplace(connection, Registration{.guid = guid});
        if (guid) {
            guids_.insert(*guid);
        }
    });
}

void Endpoint::unregisterConnection(std::shared_ptr<MinecraftConnection> connection)
{
    asio::post(strand_, [this, connection = std::move(connection)] {
        const auto registration = connections_.find(connection);
        if (registration == connections_.end()) {
            return;
        }
        unroute(connection, registration->second);
        if (registration->second.guid) {
            guids_.erase(*registration->second.guid);
        }
        if (registration->second.half_open) {
            half_open_.erase(*registration->second.half_open);
        }
        if (const auto session = ufrags_.find(registration->second.ufrag);
            session != ufrags_.end() && session->second == connection) {
            ufrags_.erase(session);
        }
        connections_.erase(registration);
    });
}

void Endpoint::activated(std::shared_ptr<MinecraftConnection> connection)
{
    asio::post(strand_, [this, connection = std::move(connection)] {
        const auto registration = connections_.find(connection);
        if (registration == connections_.end() || !registration->second.half_open) {
            return;
        }
        half_open_.erase(*registration->second.half_open);
        registration->second.half_open.reset();
    });
}

void Endpoint::addRoute(const asio::ip::udp::endpoint &remote, std::shared_ptr<MinecraftConnection> connection)
{
    asio::post(strand_, [this, remote, connection = std::move(connection)] {
        const auto registration = connections_.find(connection);
        if (registration == connections_.end()) {
            return;
        }
        if (!std::ranges::contains(registration->second.routes, remote)) {
            registration->second.routes.push_back(remote);
        }
        routes_.insert_or_assign(remote, connection);
    });
}

void Endpoint::removeRoute(const asio::ip::udp::endpoint &remote, std::shared_ptr<MinecraftConnection> connection)
{
    asio::post(strand_, [this, remote, connection = std::move(connection)] {
        const auto registration = connections_.find(connection);
        if (registration == connections_.end()) {
            return;
        }
        std::erase(registration->second.routes, remote);
        if (const auto route = routes_.find(remote); route != routes_.end() && route->second == connection) {
            routes_.erase(route);
        }
    });
}

ServerPing Endpoint::getServerPing(const nethernet::PingRequest &request)
{
    return server_.getServerListPingHandler().getInitialPing(request.protocol);
}

void Endpoint::onJoin(nethernet::JoinRequest request, nethernet::JoinResponse response)
{
    asio::co_spawn(strand_, join(std::move(request), std::move(response)), asio::detached);
}

asio::awaitable<void> Endpoint::join(nethernet::JoinRequest request, nethernet::JoinResponse response)
{
    constexpr std::chrono::seconds resolve_timeout{2};

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
        asio::ip::udp::resolver resolver(strand_);
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
    else if (ifaddrs *list = nullptr; ::getifaddrs(&list) == 0) {
        const std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> owned(list, ::freeifaddrs);
        for (const auto *entry = list; entry != nullptr; entry = entry->ifa_next) {
            const auto family = entry->ifa_addr == nullptr ? AF_UNSPEC : entry->ifa_addr->sa_family;
            if (family != AF_INET && (family != AF_INET6 || !local.address().is_v6())) {
                continue;
            }
            asio::ip::udp::endpoint address;
            const auto size = family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
            std::memcpy(address.data(), entry->ifa_addr, size);
            address.resize(size);
            interfaces.push_back(address.address());
        }
    }

    const auto port = local.port();
    auto candidates = nethernet::gatherCandidates(reached, request.port.value_or(port), interfaces, port);
    auto credentials = rtc::IceCredentials::generate();
    while (ufrags_.contains(credentials.ufrag)) {
        credentials = rtc::IceCredentials::generate();
    }
    const auto ufrag = credentials.ufrag;
    const auto connection = std::make_shared<NetherNetConnection<nethernet::NetworkSession>>(
        *this, request.network_id, *offer, std::move(credentials), certificate_, session_config_,
        MinecraftConnection::Clock::now());
    connection->setSessionHandler(
        std::make_shared<ClientPlaySessionHandler>(std::make_shared<Player>(server_, connection)));
    connections_.emplace(connection, Registration{.ufrag = ufrag});
    ufrags_.emplace(ufrag, connection);
    connection->answer(*offer, std::move(candidates), next_session_id_++, identity_, std::move(response));
}

void Endpoint::close()
{
    closed_ = true;
    asio::error_code ignored;
    std::ignore = socket_->cancel(ignored);
    signaling_->close();
    for (const auto &[connection, registration] : std::exchange(connections_, {})) {
        connection->shutdown();
    }
    routes_.clear();
    ufrags_.clear();
    guids_.clear();
    half_open_.clear();
}

}  // namespace endgate
