#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <spdlog/logger.h>

#include "endgate/network/discovery.h"
#include "endgate/network/endpoint.h"
#include "endgate/network/offline.h"

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
    void lanDiscoveryBind(const asio::ip::address &address);
    void start();
    void shutdown();
    void awaitTermination();

private:
    asio::awaitable<void> accept(asio::ip::tcp::acceptor &acceptor);
    asio::awaitable<void> receive(asio::ip::udp::socket &socket);
    asio::awaitable<void> receiveLanDiscovery(asio::ip::udp::socket &socket);

    ProxyServer &server_;
    spdlog::logger &logger_;
    OfflineMessageHandler offline_;
    LanDiscoveryHandler lan_discovery_;
    asio::io_context io_context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    asio::signal_set signals_;
    unsigned int threads_;
    std::deque<Endpoint> endpoints_;
    std::vector<std::jthread> workers_;
};

}  // namespace endgate
