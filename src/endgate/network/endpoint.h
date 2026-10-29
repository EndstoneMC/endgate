#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ssl/context.hpp>
#include <spdlog/logger.h>

#include "endgate/awaitable.h"
#include "endgate/connection/connection.h"
#include "endgate/network/eventloop.h"
#include "endgate/network/nethernet/dns.h"
#include "endgate/network/nethernet/identity.h"
#include "endgate/network/nethernet/session.h"
#include "endgate/network/nethernet/signaling.h"
#include "endgate/network/ping.h"
#include "endgate/network/raknet/offline.h"
#include "endgate/network/registry.h"
#include "endgate/network/rtc/certificate.h"

namespace endgate {

class ProxyServer;

/**
 * One event loop's share of the listening port: its UDP socket RakNet and NetherNet share, its NetherNet
 * HTTP signalling acceptor on TCP at the same port number, and every connection that arrived on them or
 * was opened for one that did. Everything here runs on the endpoint's loop, so the tables need no
 * lock; what the endpoints of one port share lives in the Registry.
 *
 * @see com.velocitypowered.proxy.network.Endpoint
 * @see io.netty.channel.ServerChannel
 */
class Endpoint : private raknet::OfflineHandler::Listener, private nethernet::JoinHandler {
public:
    Endpoint(ProxyServer &server, EventLoop &event_loop, Registry &registry,
             std::shared_ptr<asio::ip::udp::socket> socket, std::uint64_t guid,
             std::span<const std::byte> offline_ping_response,
             std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> identity,
             std::shared_ptr<asio::ssl::context> tls);
    Endpoint(const Endpoint &) = delete;
    Endpoint &operator=(const Endpoint &) = delete;
    Endpoint(Endpoint &&) = delete;
    Endpoint &operator=(Endpoint &&) = delete;
    ~Endpoint() override;

    [[nodiscard]] std::string getLocalAddress() const;
    [[nodiscard]] EventLoop &getEventLoop() const { return event_loop_; }
    [[nodiscard]] const std::shared_ptr<asio::ip::udp::socket> &getSocket() const { return socket_; }
    [[nodiscard]] ProxyServer &getServer() const { return server_; }
    [[nodiscard]] const std::shared_ptr<const rtc::Certificate> &getCertificate() const { return certificate_; }
    [[nodiscard]] const std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> &getIdentity() const
    {
        return identity_;
    }

    void listen(const asio::ip::tcp::endpoint &endpoint);

    void start();
    void close();
    void forward(const ByteBuf &datagram, const asio::ip::udp::endpoint &sender, Endpoint &origin);
    void invalidate(const asio::ip::udp::endpoint &sender, Endpoint *owner);

    [[nodiscard]] std::size_t getConnectionCount() const { return connections_.size(); }
    [[nodiscard]] std::size_t getForwardCount() const { return forwards_.size(); }

    /**
     * Keeps track of a connection the endpoint did not accept itself, so it is shut down with the
     * endpoint and no player can connect with its GUID.
     *
     * @see com.velocitypowered.proxy.VelocityServer#registerConnection
     */
    void registerConnection(const std::shared_ptr<MinecraftConnection> &connection, std::optional<std::uint64_t> guid);

    /**
     * @see com.velocitypowered.proxy.VelocityServer#unregisterConnection
     */
    void unregisterConnection(const std::shared_ptr<MinecraftConnection> &connection);

    void activated(const std::shared_ptr<MinecraftConnection> &connection);
    void addRoute(const asio::ip::udp::endpoint &remote, const std::shared_ptr<MinecraftConnection> &connection);
    void removeRoute(const asio::ip::udp::endpoint &remote, const std::shared_ptr<MinecraftConnection> &connection);

private:
    struct Registration {
        std::vector<asio::ip::udp::endpoint> routes;
        std::optional<std::uint64_t> guid;
        std::optional<std::uint16_t> system_index;
        std::optional<std::uint64_t> half_open;
        std::string ufrag;
    };

    [[nodiscard]] ServerPing getServerPing(const nethernet::PingRequest &request) override;
    void onJoin(nethernet::JoinRequest request, nethernet::JoinResponse response) override;
    Awaitable<void> join(nethernet::JoinRequest request, nethernet::JoinResponse response);

    Awaitable<void> receive();
    void handleDatagram(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender);
    void accept(std::span<const std::byte> datagram, const raknet::SystemAddress &sender);
    void sendTo(const raknet::SystemAddress &address, ByteBufView datagram) override;
    void unroute(const std::shared_ptr<MinecraftConnection> &connection, Registration &registration);
    [[nodiscard]] std::uint16_t getFreeSystemIndex() const;

    ProxyServer &server_;
    spdlog::logger &logger_;
    EventLoop &event_loop_;
    Registry &registry_;
    std::shared_ptr<asio::ip::udp::socket> socket_;
    raknet::OfflineHandler offline_;
    std::shared_ptr<const rtc::Certificate> certificate_;
    std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> identity_;
    nethernet::NetworkSession::Config session_config_;
    std::shared_ptr<nethernet::HttpSignalingServer> signaling_;
    nethernet::DnsCache dns_;
    std::unordered_map<std::shared_ptr<MinecraftConnection>, Registration> connections_;
    std::unordered_map<asio::ip::udp::endpoint, std::shared_ptr<MinecraftConnection>> routes_;
    std::unordered_map<std::string, std::shared_ptr<MinecraftConnection>> ufrags_;
    std::unordered_map<asio::ip::udp::endpoint, Endpoint *> forwards_;
    std::deque<asio::ip::udp::endpoint> forward_order_;
    std::unordered_map<std::string, std::shared_ptr<ScheduledFuture>> expiring_;
    std::map<std::uint64_t, std::shared_ptr<MinecraftConnection>> half_open_;
    std::uint64_t next_half_open_{0};
    bool closed_{false};
};

}  // namespace endgate
