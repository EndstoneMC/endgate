#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "endgate/awaitable.h"
#include "endgate/crypto/jose.h"
#include "endgate/network/eventloop.h"
#include "endgate/network/http.h"

namespace endgate {

/**
 * Fetches the keys Minecraft Services signs player tokens with, through its discovery service and
 * OpenID configuration, and measures how far the service's clock is from ours. It fetches again a
 * week after the last fetch started, and sooner when a token fails its signature. Its keys and clock
 * may be read from any thread.
 *
 * @see MinecraftServiceKeyManager
 */
class MinecraftServiceKeyManager : public std::enable_shared_from_this<MinecraftServiceKeyManager> {
public:
    static constexpr std::string_view DISCOVERY_URI =
        "https://client.discovery.minecraft-services.net/api/v1.0/discovery/MinecraftDedicatedServer/builds/";

    MinecraftServiceKeyManager(HttpClient client, std::string user_agent, std::string discovery_uri);
    MinecraftServiceKeyManager(const MinecraftServiceKeyManager &) = delete;
    MinecraftServiceKeyManager &operator=(const MinecraftServiceKeyManager &) = delete;
    MinecraftServiceKeyManager(MinecraftServiceKeyManager &&) = delete;
    MinecraftServiceKeyManager &operator=(MinecraftServiceKeyManager &&) = delete;
    ~MinecraftServiceKeyManager();

    /**
     * Fetches the keys and waits for that first fetch to end, then keeps them fresh on the client's
     * event loop.
     */
    void start();

    void shutdown();

    [[nodiscard]] bool hasKeys() const;
    [[nodiscard]] PublicKey getKey(std::string_view key_id) const;
    [[nodiscard]] std::string getIssuer() const;

    /**
     * Local time corrected by the offset the last fetch measured against the service's clock.
     */
    [[nodiscard]] std::chrono::system_clock::time_point now() const;

    /**
     * Fetches again when the last fetch started more than ten minutes ago.
     *
     * @see MinecraftServiceKeyManager::_fullFetch
     */
    void onSignatureFailure();

private:
    using Keys = std::map<std::string, PublicKey, std::less<>>;

    Awaitable<void> fetch();
    void scheduleNextFetch();

    spdlog::logger &logger_;
    HttpClient client_;
    std::string user_agent_;
    std::string discovery_uri_;
    mutable std::mutex mutex_;
    Keys keys_;
    std::string issuer_;
    std::chrono::system_clock::duration offset_{};
    std::chrono::steady_clock::time_point last_fetch_started_;
    bool fetching_{false};
    bool shut_down_{false};
    bool unreachable_ = false;
    std::shared_ptr<ScheduledFuture> next_fetch_;
};

}  // namespace endgate
