#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "endgate/backend.h"
#include "endgate/config.h"
#include "endgate/console.h"
#include "endgate/network/manager.h"
#include "endgate/network/ping.h"
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
    [[nodiscard]] static ProxyVersion getVersion();
    [[nodiscard]] Console &getConsoleCommandSource();
    [[nodiscard]] const ServerListPingHandler &getServerListPingHandler() const;
    [[nodiscard]] const ProxyConfig &getConfiguration() const;

    /**
     * @see com.velocitypowered.api.proxy.ProxyServer#getServer
     */
    [[nodiscard]] BackendServer *getServer(std::string_view name);

private:
    spdlog::logger &logger_;
    ConnectionManager cm_;
    Console console_;
    ProxyConfig configuration_;
    ServerListPingHandler ping_handler_;
    std::map<std::string, BackendServer, std::less<>> servers_;
    std::atomic<bool> shutdown_in_progress_{false};
    std::atomic<bool> shutdown_{false};
};

}  // namespace endgate
