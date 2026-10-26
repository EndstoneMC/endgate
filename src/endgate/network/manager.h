#pragma once

#include <cstdint>
#include <deque>
#include <thread>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <spdlog/logger.h>

#include "endgate/network/endpoint.h"
#include "endgate/network/offline.h"
#include "endgate/network/legacy/raknet/BitStream.h"
#include "endgate/network/legacy/raknet/MTUSize.h"
#include "endgate/network/legacy/raknet/RakPeer.h"

namespace endgate {

class ProxyServer;

/**
 * Manages the endpoints endgate listens on, along with the threads running them.
 *
 * @see com.velocitypowered.proxy.network.ConnectionManager
 */
class ConnectionManager {
public:
    explicit ConnectionManager(ProxyServer &server);
    ~ConnectionManager();

    void logChannelInformation();
    bool bind(const asio::ip::address &address, std::uint16_t port);
    void start();
    void shutdown();
    void awaitTermination();

private:
    asio::awaitable<void> receive(Endpoint &endpoint);

    /// RakNet's own cadence. Arrival drives transmission - the congestion window is ACK-clocked and has
    /// no pacer - so this only drives what arrival cannot: resend timers, the delayed-ACK deadline, the
    /// keepalive that makes an idle connection detectable, and the half-open reaper.
    void tick();

    static constexpr auto TICK_INTERVAL = std::chrono::milliseconds{10};

    ProxyServer &server_;
    spdlog::logger &logger_;
    OfflineMessageHandler offline_;
    asio::io_context io_context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    asio::signal_set signals_;
    unsigned int threads_;
    std::deque<Endpoint> endpoints_;
    std::vector<std::jthread> workers_;

    RakNet::RakPeer peer_;
    RakNet::BitStream scratch_{MAXIMUM_MTU_SIZE};
    asio::steady_timer timer_;
    bool stopping_{false};
};

}  // namespace endgate
