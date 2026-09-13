#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "endgate/console.h"
#include "endgate/version.h"

namespace endgate {

/**
 * The proxy itself, and the entry point to everything it owns.
 *
 * @see com.velocitypowered.api.proxy.ProxyServer
 * @see com.velocitypowered.proxy.VelocityServer
 */
class ProxyServer {
public:
    ProxyServer();

    void start();
    void shutdown(bool explicit_exit);
    void awaitProxyShutdown();

    [[nodiscard]] bool isShutdown() const;
    [[nodiscard]] bool isShuttingDown() const;
    [[nodiscard]] ProxyVersion getVersion() const;
    [[nodiscard]] Console &getConsoleCommandSource();

private:
    Console console_;
    std::atomic_bool shutdown_in_progress_{false};
    std::atomic_bool shutdown_{false};
    std::mutex shutdown_mutex_;
    std::condition_variable shutdown_cv_;
};

}  // namespace endgate
