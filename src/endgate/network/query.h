#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>

#include <asio/ip/address.hpp>
#include <asio/ip/udp.hpp>
#include <spdlog/logger.h>

#include "endgate/awaitable.h"
#include "endgate/buffer/bytebuf.h"
#include "endgate/network/eventloop.h"

namespace endgate {

class ProxyServer;

/**
 * Implements the GameSpy 4 query protocol. It answers on the game port's own sockets when the query
 * port is the bind port, and otherwise on a socket of its own.
 *
 * @see com.velocitypowered.proxy.protocol.netty.GameSpyQueryHandler
 */
class GameSpyQueryHandler {
public:
    explicit GameSpyQueryHandler(ProxyServer &server);
    GameSpyQueryHandler(const GameSpyQueryHandler &) = delete;
    GameSpyQueryHandler &operator=(const GameSpyQueryHandler &) = delete;
    GameSpyQueryHandler(GameSpyQueryHandler &&) = delete;
    GameSpyQueryHandler &operator=(GameSpyQueryHandler &&) = delete;
    ~GameSpyQueryHandler() = default;

    /**
     * @see com.velocitypowered.proxy.network.ConnectionManager#queryBind
     */
    bool queryBind(EventLoop &event_loop, const asio::ip::address &address, std::uint16_t port);
    void close();

    [[nodiscard]] static bool isQuery(std::span<const std::byte> datagram);

    void channelRead0(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender,
                      asio::ip::udp::socket &socket);

private:
    Awaitable<void> receive(std::shared_ptr<asio::ip::udp::socket> socket);
    [[nodiscard]] ByteBuf createStatResponse(std::int32_t session_id, bool is_basic);

    ProxyServer &server_;
    spdlog::logger &logger_;
    std::mutex mutex_;
    std::unordered_map<asio::ip::address, std::pair<std::int32_t, std::chrono::steady_clock::time_point>> sessions_;
    std::shared_ptr<asio::ip::udp::socket> socket_;
};

}  // namespace endgate
