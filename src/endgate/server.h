#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <asio/ip/address.hpp>
#include <spdlog/logger.h>

#include "endgate/auth/keys.h"
#include "endgate/backend.h"
#include "endgate/config.h"
#include "endgate/console.h"
#include "endgate/network/http.h"
#include "endgate/network/manager.h"
#include "endgate/network/ping.h"
#include "endgate/util/ratelimit.h"
#include "endgate/util/uuid.h"
#include "endgate/version.h"

namespace endgate {

class Player;

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
     * @see com.velocitypowered.proxy.VelocityServer#createHttpClient
     */
    [[nodiscard]] HttpClient createHttpClient();

    /**
     * The keys online mode checks player tokens against, or nullptr in offline mode.
     */
    [[nodiscard]] MinecraftServiceKeyManager *getMinecraftServiceKeyManager() const { return keys_.get(); }

    /**
     * @see com.velocitypowered.api.proxy.ProxyServer#getServer
     */
    [[nodiscard]] BackendServer *getServer(std::string_view name);

    /**
     * @see com.velocitypowered.proxy.VelocityServer#getIpAttemptLimiter
     */
    [[nodiscard]] Ratelimiter<asio::ip::address> &getIpAttemptLimiter() const;

    /**
     * Checks if the connection can be registered with the proxy.
     *
     * @see com.velocitypowered.proxy.VelocityServer#canRegisterConnection
     */
    [[nodiscard]] bool canRegisterConnection(const Player &connection);

    /**
     * Attempts to register the connection with the proxy.
     *
     * @see com.velocitypowered.proxy.VelocityServer#registerConnection
     */
    [[nodiscard]] bool registerConnection(const std::shared_ptr<Player> &connection);

    /**
     * Unregisters the given player from the proxy.
     *
     * @see com.velocitypowered.proxy.VelocityServer#unregisterConnection
     */
    void unregisterConnection(const Player &connection);

private:
    struct CaseInsensitiveLess {
        bool operator()(std::string_view left, std::string_view right) const;
    };

    spdlog::logger &logger_;
    ConnectionManager cm_;
    Console console_;
    ProxyConfig configuration_;
    ServerListPingHandler ping_handler_;
    std::map<std::string, BackendServer, std::less<>> servers_;
    std::shared_ptr<MinecraftServiceKeyManager> keys_;
    std::unique_ptr<Ratelimiter<asio::ip::address>> ip_attempt_limiter_;
    std::mutex connections_mutex_;
    std::map<UUID, std::shared_ptr<Player>> connections_by_uuid_;
    std::map<std::string, std::shared_ptr<Player>, CaseInsensitiveLess> connections_by_name_;
    std::atomic<bool> shutdown_in_progress_{false};
    std::atomic<bool> shutdown_{false};
};

}  // namespace endgate
