#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/strand.hpp>
#include <spdlog/logger.h>

#include "endgate/network/buffer.h"
#include "endgate/network/raknet/connection.h"
#include "endgate/network/raknet/offline.h"

namespace endgate {

class ProxyServer;

/**
 * One listening socket, the executor its traffic is serialised on, and the connections that arrived
 * on it, with the backend connections opened for them.
 *
 * @see com.velocitypowered.proxy.network.Endpoint
 * @see io.netty.channel.EventLoop
 */
class Endpoint : private raknet::OfflineHandler::Listener {
public:
    using Executor = asio::strand<asio::io_context::executor_type>;

    Endpoint(ProxyServer &server, asio::io_context &io_context, asio::ip::udp::socket socket, std::uint64_t guid,
             std::span<const std::byte> offline_ping_response);
    Endpoint(const Endpoint &) = delete;
    Endpoint &operator=(const Endpoint &) = delete;
    Endpoint(Endpoint &&) = delete;
    Endpoint &operator=(Endpoint &&) = delete;
    ~Endpoint();

    [[nodiscard]] std::string getLocalAddress() const;
    [[nodiscard]] const Executor &getExecutor() const { return strand_; }

    void start();
    void close();

private:
    class Session;

    asio::awaitable<void> receive();
    asio::awaitable<void> receive(std::shared_ptr<Session> backend);

    void handleDatagram(std::span<const std::byte> datagram, const raknet::SystemAddress &sender);
    void sendTo(const raknet::SystemAddress &address, std::span<const std::byte> datagram) override;

    /// @see io.netty.channel.ChannelInboundHandler#channelActive
    void channelActive(Session &session);

    /// @see io.netty.channel.ChannelInboundHandler#channelRead
    void channelRead(Session &session, ByteBuf message);

    /// @see io.netty.channel.ChannelInboundHandler#channelInactive
    void channelInactive(Session &session, raknet::DisconnectReason reason);

    void connectBackend(const std::shared_ptr<Session> &player);
    [[nodiscard]] bool allowIncomingConnections() const;
    [[nodiscard]] bool isGuidInUse(std::uint64_t guid) const;
    [[nodiscard]] std::uint16_t getFreeSystemIndex() const;
    [[nodiscard]] raknet::ConnectionConfiguration getConnectionConfiguration() const;

    ProxyServer &server_;
    spdlog::logger &logger_;
    asio::io_context &io_context_;
    asio::ip::udp::socket socket_;
    Executor strand_;
    raknet::OfflineHandler offline_;
    std::unordered_map<raknet::SystemAddress, std::shared_ptr<Session>> players_;
    std::unordered_set<std::shared_ptr<Session>> backends_;
    bool closed_{false};
};

}  // namespace endgate
