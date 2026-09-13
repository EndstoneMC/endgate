#pragma once

#include <atomic>

#include <spdlog/logger.h>

#include "endgate/config.h"
#include "endgate/console.h"
#include "endgate/network/manager.h"
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

    bool start();
    void shutdown(bool explicit_exit);
    void awaitProxyShutdown();

    [[nodiscard]] bool isShutdown() const;
    [[nodiscard]] bool isShuttingDown() const;
    [[nodiscard]] ProxyVersion getVersion() const;
    [[nodiscard]] Console &getConsoleCommandSource();
    [[nodiscard]] const ProxyConfig &getConfiguration() const;

private:
    spdlog::logger &logger_;
    ConnectionManager cm_;
    Console console_;
    ProxyConfig configuration_;
    std::atomic_bool shutdown_in_progress_{false};
    std::atomic_bool shutdown_{false};
};

}  // namespace endgate
