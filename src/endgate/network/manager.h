#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>

#include <asio/signal_set.hpp>
#include <spdlog/logger.h>

#include "endgate/network/endpoint.h"
#include "endgate/network/eventloop.h"
#include "endgate/network/http.h"
#include "endgate/network/nethernet/identity.h"
#include "endgate/network/offline.h"
#include "endgate/network/registry.h"

namespace endgate {

class ProxyServer;

/**
 * Manages the endpoints endgate listens on, one per event loop, along with the event loop group running
 * them.
 *
 * @see com.velocitypowered.proxy.network.ConnectionManager
 * @see io.netty.channel.MultiThreadIoEventLoopGroup
 */
class ConnectionManager {
public:
    explicit ConnectionManager(ProxyServer &server);
    ~ConnectionManager();

    void logChannelInformation();

    /**
     * @see com.velocitypowered.proxy.network.ConnectionManager#createHttpClient
     */
    [[nodiscard]] HttpClient createHttpClient();
    bool bind(const asio::ip::address &address, std::uint16_t port);
    void start();
    void shutdown();
    void awaitTermination();

private:
    ProxyServer &server_;
    spdlog::logger &logger_;
    OfflineMessageHandler offline_;
    std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> identity_;
    std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> offer_identity_;
    Registry registry_;
    EventLoopGroup group_;
    asio::signal_set signals_;
    std::deque<Endpoint> endpoints_;
    std::atomic<bool> shut_down_{false};
};

}  // namespace endgate
