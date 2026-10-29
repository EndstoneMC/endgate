#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/strand.hpp>
#include <spdlog/logger.h>

#include "endgate/connection/connection.h"
#include "endgate/network/raknet/offline.h"

namespace endgate {

class ProxyServer;

/**
 * One listening port: the UDP socket RakNet and NetherNet share, and every connection that arrived on
 * it or was opened for one that did. The endpoint's strand receives each datagram and hands it to the
 * connection its sender is routed to, and creates and removes connections; each connection runs on a
 * strand of its own and sends on the shared socket itself.
 *
 * @see com.velocitypowered.proxy.network.Endpoint
 */
class Endpoint : private raknet::OfflineHandler::Listener {
public:
    using Executor = asio::strand<asio::io_context::executor_type>;

    Endpoint(ProxyServer &server, asio::io_context &io_context, std::shared_ptr<asio::ip::udp::socket> socket,
             std::uint64_t guid, std::span<const std::byte> offline_ping_response);
    Endpoint(const Endpoint &) = delete;
    Endpoint &operator=(const Endpoint &) = delete;
    Endpoint(Endpoint &&) = delete;
    Endpoint &operator=(Endpoint &&) = delete;
    ~Endpoint();

    [[nodiscard]] std::string getLocalAddress() const;
    [[nodiscard]] const Executor &getExecutor() const { return strand_; }
    [[nodiscard]] asio::io_context &getIoContext() const { return io_context_; }
    [[nodiscard]] const std::shared_ptr<asio::ip::udp::socket> &getSocket() const { return socket_; }
    [[nodiscard]] ProxyServer &getServer() const { return server_; }

    void start();
    void close();

    /**
     * Keeps track of a connection the endpoint did not accept itself, so it is shut down with the
     * endpoint and no player can connect with its GUID.
     *
     * @see com.velocitypowered.proxy.VelocityServer#registerConnection
     */
    void registerConnection(std::shared_ptr<MinecraftConnection> connection, std::optional<std::uint64_t> guid);

    /**
     * @see com.velocitypowered.proxy.VelocityServer#unregisterConnection
     */
    void unregisterConnection(std::shared_ptr<MinecraftConnection> connection);

    void activated(std::shared_ptr<MinecraftConnection> connection);
    void addRoute(const asio::ip::udp::endpoint &remote, std::shared_ptr<MinecraftConnection> connection);
    void removeRoute(const asio::ip::udp::endpoint &remote, std::shared_ptr<MinecraftConnection> connection);

private:
    struct Registration {
        std::vector<asio::ip::udp::endpoint> routes;
        std::optional<std::uint64_t> guid;
        std::optional<std::uint16_t> system_index;
        std::optional<std::uint64_t> half_open;
    };

    asio::awaitable<void> receive();
    void handleDatagram(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender);
    void accept(std::span<const std::byte> datagram, const raknet::SystemAddress &sender);
    void sendTo(const raknet::SystemAddress &address, std::span<const std::byte> datagram) override;
    void unroute(const std::shared_ptr<MinecraftConnection> &connection, Registration &registration);
    [[nodiscard]] std::uint16_t getFreeSystemIndex() const;

    ProxyServer &server_;
    spdlog::logger &logger_;
    asio::io_context &io_context_;
    std::shared_ptr<asio::ip::udp::socket> socket_;
    Executor strand_;
    raknet::OfflineHandler offline_;
    std::unordered_map<std::shared_ptr<MinecraftConnection>, Registration> connections_;
    std::unordered_map<asio::ip::udp::endpoint, std::shared_ptr<MinecraftConnection>> routes_;
    std::unordered_set<std::uint64_t> guids_;
    std::map<std::uint64_t, std::shared_ptr<MinecraftConnection>> half_open_;
    std::uint64_t next_half_open_{0};
    bool closed_{false};
};

}  // namespace endgate
