#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/ip/address.hpp>
#include <asio/ip/udp.hpp>
#include <spdlog/logger.h>

#include "endgate/auth/keys.h"
#include "endgate/awaitable.h"
#include "endgate/backend.h"
#include "endgate/command/manager.h"
#include "endgate/config.h"
#include "endgate/console.h"
#include "endgate/metrics.h"
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
    /**
     * Shuts down the proxy, kicking players with the reason.
     *
     * @see com.velocitypowered.proxy.VelocityServer#shutdown(boolean, Component)
     */
    void shutdown(bool explicit_exit, const std::string &reason = "Proxy shutting down.");
    void awaitProxyShutdown();

    /**
     * Reads endgate.toml again and applies it, keeping the current configuration when it is invalid. Players on a
     * server whose address changed are moved to the next server they can try, and kicked when there is none.
     * Blocks until they are moved, so it must not run on an event loop.
     *
     * @see com.velocitypowered.proxy.VelocityServer#reloadConfiguration
     */
    [[nodiscard]] bool reloadConfiguration();

    [[nodiscard]] bool isShutdown() const;
    [[nodiscard]] bool isShuttingDown() const;
    [[nodiscard]] static ProxyVersion getVersion();
    [[nodiscard]] Console &getConsoleCommandSource();

    /**
     * @see com.velocitypowered.api.proxy.ProxyServer#getCommandManager
     */
    [[nodiscard]] CommandManager &getCommandManager();
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
     * Registers a server, or returns the one already registered under its name.
     *
     * @see com.velocitypowered.api.proxy.ProxyServer#registerServer
     */
    BackendServer &registerServer(ServerInfo info);

    /**
     * @see com.velocitypowered.api.proxy.ProxyServer#getServer
     */
    [[nodiscard]] BackendServer *getServer(std::string_view name);

    /**
     * @see com.velocitypowered.api.proxy.ProxyServer#getAllServers
     */
    [[nodiscard]] std::vector<BackendServer *> getAllServers();

    /**
     * Retrieves the player currently connected to this proxy by their Minecraft username.
     *
     * @see com.velocitypowered.api.proxy.ProxyServer#getPlayer(String)
     */
    [[nodiscard]] std::shared_ptr<Player> getPlayer(std::string_view username);

    /**
     * Returns the number of players currently connected to this proxy.
     *
     * @see com.velocitypowered.api.proxy.ProxyServer#getPlayerCount
     */
    [[nodiscard]] std::size_t getPlayerCount();

    /**
     * Retrieves all players currently connected to this proxy.
     *
     * @see com.velocitypowered.api.proxy.ProxyServer#getAllPlayers
     */
    [[nodiscard]] std::vector<std::shared_ptr<Player>> getAllPlayers();

    /**
     * @see com.velocitypowered.proxy.VelocityServer#getIpAttemptLimiter
     */
    [[nodiscard]] Ratelimiter<asio::ip::address> &getIpAttemptLimiter() const;

    /**
     * @see com.velocitypowered.proxy.VelocityServer#getCommandRateLimiter
     */
    [[nodiscard]] Ratelimiter<UUID> &getCommandRateLimiter() const;

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

    void updateSoftEnums();

    [[nodiscard]] static std::optional<asio::ip::udp::endpoint> parseBind(const std::string &bind);
    static Awaitable<void> evacuate(std::shared_ptr<Player> player, std::vector<const BackendServer *> replaced,
                                    std::latch *moved);

    spdlog::logger &logger_;
    std::mutex reload_mutex_;
    std::vector<std::unique_ptr<ProxyConfig>> configurations_;
    std::atomic<const ProxyConfig *> configuration_ = nullptr;
    std::vector<std::unique_ptr<Ratelimiter<asio::ip::address>>> ip_attempt_limiters_;
    std::atomic<Ratelimiter<asio::ip::address> *> ip_attempt_limiter_ = nullptr;
    std::vector<std::unique_ptr<Ratelimiter<UUID>>> command_rate_limiters_;
    std::atomic<Ratelimiter<UUID> *> command_rate_limiter_ = nullptr;
    std::mutex servers_mutex_;
    std::map<std::string, std::unique_ptr<BackendServer>, std::less<>> servers_;
    std::vector<std::unique_ptr<BackendServer>> retired_servers_;
    ConnectionManager cm_;
    CommandManager command_manager_;
    std::shared_ptr<Console> console_;
    ServerListPingHandler ping_handler_;
    std::shared_ptr<MinecraftServiceKeyManager> keys_;
    std::unique_ptr<Metrics> metrics_;
    std::mutex connections_mutex_;
    std::condition_variable connections_changed_;
    std::map<UUID, std::shared_ptr<Player>> connections_by_uuid_;
    std::map<std::string, std::shared_ptr<Player>, CaseInsensitiveLess> connections_by_name_;
    std::atomic<bool> shutdown_in_progress_{false};
    std::atomic<bool> shutdown_{false};
    std::jthread shutdown_thread_;
};

}  // namespace endgate
