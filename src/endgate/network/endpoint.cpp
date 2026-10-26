#include "endgate/network/endpoint.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <netinet/in.h>
#include <sys/socket.h>

#include "endgate/config.h"
#include "endgate/logging/manager.h"
#include "endgate/server.h"

namespace endgate {

namespace {

std::string toString(const raknet::SystemAddress &address)
{
    return address.address().to_string() + ":" + std::to_string(address.port());
}

void setDontFragment(asio::ip::udp::socket &socket, bool dont_fragment)
{
    asio::error_code ec;
    if (socket.local_endpoint(ec).protocol() == asio::ip::udp::v4()) {
        const int value = dont_fragment ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
        static_cast<void>(::setsockopt(socket.native_handle(), IPPROTO_IP, IP_MTU_DISCOVER, &value, sizeof(value)));
        return;
    }
    const int value = dont_fragment ? IPV6_PMTUDISC_DO : IPV6_PMTUDISC_DONT;
    static_cast<void>(::setsockopt(socket.native_handle(), IPPROTO_IPV6, IPV6_MTU_DISCOVER, &value, sizeof(value)));
}

}  // namespace

/**
 * @see com.velocitypowered.proxy.connection.MinecraftConnection
 */
class Endpoint::Session : public raknet::Connection::Listener, public std::enable_shared_from_this<Session> {
public:
    Session(Endpoint &endpoint, const raknet::SystemAddress &address, const raknet::OpenConnectionRequest2 &request,
            std::uint16_t system_index, raknet::TimePoint now)
        : endpoint_(endpoint), address_(address), key_(toString(address)), timer_(endpoint.strand_),
          connection_(*this, endpoint.getConnectionConfiguration(), address, request, system_index, now)
    {
    }

    Session(Endpoint &endpoint, const raknet::SystemAddress &address, asio::ip::udp::socket socket,
            raknet::TimePoint now)
        : endpoint_(endpoint), address_(address), key_(toString(address)), socket_(std::move(socket)),
          timer_(endpoint.strand_), connection_(*this, endpoint.getConnectionConfiguration(), address, now)
    {
    }

    void sendDatagram(std::span<const std::byte> datagram) override
    {
        asio::error_code ignored;
        if (socket_) {
            socket_->send(asio::buffer(datagram), 0, ignored);
            return;
        }
        endpoint_.socket_.send_to(asio::buffer(datagram), address_, 0, ignored);
    }

    bool sendMtuProbe(std::span<const std::byte> datagram) override
    {
        if (!socket_) {
            sendDatagram(datagram);
            return true;
        }
        asio::error_code ec;
        setDontFragment(*socket_, true);
        socket_->send(asio::buffer(datagram), 0, ec);
        setDontFragment(*socket_, false);
        return ec != asio::error::message_size;
    }

    void onConnected() override
    {
        active_ = true;
        endpoint_.channelActive(*this);
    }

    void onMessage(ByteBuf message) override { endpoint_.channelRead(*this, std::move(message)); }

    void onDisconnected(raknet::DisconnectReason reason) override
    {
        active_ = false;
        timer_.cancel();
        armed_.reset();
        endpoint_.channelInactive(*this, reason);
    }

    void onReliabilityLayerNotification(std::string_view message, bool is_error) override
    {
        if (is_error) {
            endpoint_.logger_.warn("Malformed datagram from {}: {}", key_, message);
        }
    }

    void arm()
    {
        const auto next = connection_.nextTimeout();
        if (!next) {
            if (armed_) {
                timer_.cancel();
                armed_.reset();
            }
            return;
        }
        if (armed_ && *armed_ <= *next) {
            return;
        }
        armed_ = next;
        timer_.expires_at(*next);
        timer_.async_wait([weak = weak_from_this()](const asio::error_code &ec) {
            const auto session = weak.lock();
            if (ec || !session) {
                return;
            }
            session->armed_.reset();
            session->connection_.handleTimeout(raknet::Clock::now());
            session->arm();
        });
    }

    void shutdown()
    {
        timer_.cancel();
        armed_.reset();
        if (socket_) {
            asio::error_code ignored;
            std::ignore = socket_->close(ignored);
        }
    }

    [[nodiscard]] raknet::Connection &getConnection() { return connection_; }
    [[nodiscard]] const raknet::Connection &getConnection() const { return connection_; }
    [[nodiscard]] const raknet::SystemAddress &getAddress() const { return address_; }
    [[nodiscard]] const std::string &getKey() const { return key_; }
    [[nodiscard]] bool isActive() const { return active_; }
    [[nodiscard]] bool isBackend() const { return socket_.has_value(); }
    [[nodiscard]] asio::ip::udp::socket *getSocket() { return socket_ ? &*socket_ : nullptr; }

    std::weak_ptr<Session> counterpart;

private:
    Endpoint &endpoint_;
    raknet::SystemAddress address_;
    std::string key_;
    std::optional<asio::ip::udp::socket> socket_;
    asio::steady_timer timer_;
    std::optional<raknet::TimePoint> armed_;
    bool active_{false};
    raknet::Connection connection_;
};

Endpoint::Endpoint(ProxyServer &server, asio::io_context &io_context, asio::ip::udp::socket socket, std::uint64_t guid,
                   std::span<const std::byte> offline_ping_response)
    : server_(server), logger_(LogManager::getLogger()), socket_(std::move(socket)),
      strand_(asio::make_strand(io_context)), offline_(*this, guid)
{
    offline_.setOfflinePingResponse(offline_ping_response);
}

Endpoint::~Endpoint() = default;

std::string Endpoint::getLocalAddress() const
{
    return toString(socket_.local_endpoint());
}

void Endpoint::start()
{
    asio::co_spawn(strand_, receive(), asio::detached);
}

asio::awaitable<void> Endpoint::receive()
{
    std::array<std::byte, raknet::MAXIMUM_MTU_SIZE> buffer{};
    raknet::SystemAddress sender;
    for (;;) {
        const auto [ec, size] =
            co_await socket_.async_receive_from(asio::buffer(buffer), sender, asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted || closed_) {
            co_return;
        }
        if (ec || size == 0) {
            continue;
        }
        handleDatagram(std::span{buffer}.first(size), sender);
    }
}

asio::awaitable<void> Endpoint::receive(std::shared_ptr<Session> backend) const
{
    auto *socket = backend->getSocket();
    if (socket == nullptr) {
        co_return;
    }
    std::array<std::byte, raknet::MAXIMUM_MTU_SIZE> buffer{};
    for (;;) {
        const auto [ec, size] =
            co_await socket->async_receive(asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted || !socket->is_open() || closed_) {
            co_return;
        }
        if (ec || size == 0 || backend->getConnection().isClosed()) {
            continue;
        }
        backend->getConnection().handleDatagram(std::span{buffer}.first(size), raknet::Clock::now());
        backend->arm();
    }
}

void Endpoint::sendTo(const raknet::SystemAddress &address, std::span<const std::byte> datagram)
{
    asio::error_code ignored;
    socket_.send_to(asio::buffer(datagram), address, 0, ignored);
}

void Endpoint::handleDatagram(std::span<const std::byte> datagram, const raknet::SystemAddress &sender)
{
    const auto now = raknet::Clock::now();
    const auto id = static_cast<raknet::MessageId>(datagram.front());
    const bool offline_message = raknet::isOfflineMessage(datagram);
    if (offline_message &&
        (id == raknet::MessageId::UnconnectedPing || id == raknet::MessageId::UnconnectedPingOpenConnections ||
         id == raknet::MessageId::OpenConnectionRequest1)) {
        static_cast<void>(offline_.handle(datagram, sender, allowIncomingConnections()));
        return;
    }

    if (const auto found = players_.find(sender); found != players_.end()) {
        const auto session = found->second;
        if (!session->getConnection().isClosed()) {
            session->getConnection().handleDatagram(datagram, now);
            session->arm();
            return;
        }
        players_.erase(found);
    }

    if (!offline_message || id != raknet::MessageId::OpenConnectionRequest2) {
        return;
    }
    const auto request = offline_.handle(datagram, sender, allowIncomingConnections());
    if (!request) {
        return;
    }
    if (isGuidInUse(request->client_guid)) {
        offline_.reject(sender, raknet::MessageId::AlreadyConnected);
        return;
    }
    const auto max_players = static_cast<std::size_t>(std::max(0, server_.getConfiguration().getShowMaxPlayers()));
    if (!allowIncomingConnections() || players_.size() >= max_players) {
        offline_.reject(sender, raknet::MessageId::NoFreeIncomingConnections);
        return;
    }

    const auto session = std::make_shared<Session>(*this, sender, *request, getFreeSystemIndex(), now);
    players_.emplace(sender, session);
    session->getConnection().handleDatagram(datagram, now);
    session->arm();
}

bool Endpoint::allowIncomingConnections() const
{
    const auto max_players = static_cast<std::size_t>(std::max(0, server_.getConfiguration().getShowMaxPlayers()));
    const auto connected =
        std::ranges::count_if(players_, [](const auto &entry) { return entry.second->getConnection().isConnected(); });
    return std::cmp_less(connected, max_players);
}

bool Endpoint::isGuidInUse(std::uint64_t guid) const
{
    if (guid == raknet::UNASSIGNED_RAKNET_GUID) {
        return false;
    }
    const auto uses = [guid](const std::shared_ptr<Session> &session) {
        return !session->getConnection().isClosed() && session->getConnection().getGuid() == guid;
    };
    return std::ranges::any_of(players_, [&uses](const auto &entry) { return uses(entry.second); }) ||
           std::ranges::any_of(backends_, uses);
}

std::uint16_t Endpoint::getFreeSystemIndex() const
{
    std::vector<bool> used(players_.size() + 1);
    for (const auto &entry : players_) {
        if (const auto index = entry.second->getConnection().getSystemIndex(); index < used.size()) {
            used[index] = true;
        }
    }
    return static_cast<std::uint16_t>(std::ranges::find(used, false) - used.begin());
}

raknet::ConnectionConfiguration Endpoint::getConnectionConfiguration() const
{
    return {.guid = offline_.getGuid()};
}

void Endpoint::channelActive(Session &session)
{
    if (session.isBackend()) {
        if (const auto player = session.counterpart.lock()) {
            logger_.info("Player {} is now on backend {}", player->getKey(), session.getKey());
        }
        return;
    }

    logger_.info("Connected: {}", session.getKey());
    const auto found = players_.find(session.getAddress());
    if (found != players_.end()) {
        connectBackend(found->second);
    }
}

void Endpoint::connectBackend(const std::shared_ptr<Session> &player)
{
    const auto &order = server_.getConfiguration().getAttemptConnectionOrder();
    const auto &servers = server_.getConfiguration().getServers();
    if (order.empty()) {
        logger_.warn("No servers configured to try; {} has nowhere to go", player->getKey());
        return;
    }
    const auto entry = servers.find(order.front());
    if (entry == servers.end()) {
        logger_.warn("Server \"{}\" is in try but not configured", order.front());
        return;
    }

    const auto &name = entry->second;
    const auto colon = name.rfind(':');
    const auto host = name.substr(0, colon == std::string::npos ? name.size() : colon);
    const auto port = colon == std::string::npos ? std::string{} : name.substr(colon + 1);
    asio::error_code ec;
    const auto address = asio::ip::make_address(host, ec);
    std::uint16_t port_number = 0;
    const auto [end, parsed] = std::from_chars(port.data(), port.data() + port.size(), port_number);
    if (!ec && parsed == std::errc{} && end == port.data() + port.size()) {
        openBackend(player, name, {address, port_number});
        return;
    }
    asio::co_spawn(strand_, resolveBackend(player, host, port, name), asio::detached);
}

asio::awaitable<void> Endpoint::resolveBackend(std::weak_ptr<Session> player, std::string host, std::string port,
                                               std::string name)
{
    asio::ip::udp::resolver resolver{strand_};
    const auto [ec, results] = co_await resolver.async_resolve(host, port, asio::as_tuple(asio::use_awaitable));
    const auto session = player.lock();
    if (closed_ || !session || session->getConnection().isClosed()) {
        co_return;
    }
    if (ec || results.empty()) {
        logger_.warn("Can't reach backend {} for {}", name, session->getKey());
        co_return;
    }
    openBackend(session, name, results.begin()->endpoint());
}

void Endpoint::openBackend(const std::shared_ptr<Session> &player, const std::string &name,
                           const raknet::SystemAddress &address)
{
    asio::error_code ec;
    asio::ip::udp::socket socket{strand_};
    if (socket.open(address.protocol(), ec) || socket.connect(address, ec) || socket.non_blocking(true, ec)) {
        logger_.warn("Can't reach backend {} for {}: {}", name, player->getKey(), ec.message());
        return;
    }

    const auto backend = std::make_shared<Session>(*this, address, std::move(socket), raknet::Clock::now());
    backend->counterpart = player;
    player->counterpart = backend;
    backends_.insert(backend);
    logger_.info("Opening backend {} for {}", name, player->getKey());
    asio::co_spawn(strand_, receive(backend), asio::detached);
    backend->getConnection().handleTimeout(raknet::Clock::now());
    backend->arm();
}

void Endpoint::channelRead(Session &session, ByteBuf message)
{
    const auto counterpart = session.counterpart.lock();
    if (!counterpart || !session.isActive() || !counterpart->isActive()) {
        logger_.debug("Dropping {} bytes from {}: no counterpart yet", message.length(), session.getKey());
        return;
    }
    logger_.debug("Forwarding {} bytes {} -> {}", message.length(), session.getKey(), counterpart->getKey());
    counterpart->getConnection().send(std::move(message), raknet::PacketPriority::High,
                                      raknet::PacketReliability::ReliableOrdered, 0, raknet::Clock::now());
    counterpart->arm();
}

void Endpoint::channelInactive(Session &session, raknet::DisconnectReason reason)
{
    if (reason != raknet::DisconnectReason::HandshakeTimedOut && reason != raknet::DisconnectReason::Rejected) {
        logger_.info("Disconnected: {}", session.getKey());
    }

    if (const auto counterpart = session.counterpart.lock()) {
        session.counterpart.reset();
        counterpart->counterpart.reset();
        counterpart->getConnection().close(true, raknet::Clock::now());
        counterpart->arm();
    }

    asio::post(strand_, [this, weak = session.weak_from_this()] {
        const auto finished = weak.lock();
        if (!finished) {
            return;
        }
        finished->shutdown();
        if (finished->isBackend()) {
            backends_.erase(finished);
            return;
        }
        if (const auto found = players_.find(finished->getAddress());
            found != players_.end() && found->second == finished) {
            players_.erase(found);
        }
    });
}

void Endpoint::close()
{
    closed_ = true;
    asio::error_code ignored;
    std::ignore = socket_.close(ignored);
    for (const auto &entry : players_) {
        entry.second->shutdown();
    }
    for (const auto &backend : backends_) {
        backend->shutdown();
    }
    players_.clear();
    backends_.clear();
}

}  // namespace endgate
