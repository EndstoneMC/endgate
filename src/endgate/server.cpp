#include "endgate/server.h"

#include <charconv>
#include <format>
#include <utility>

#include <asio/ip/address.hpp>

#include "endgate/logging/manager.h"
#include "endgate/network/version.h"

namespace endgate {

ProxyServer::ProxyServer()
    : logger_(LogManager::getLogger("ProxyServer")), cm_(*this), console_(*this), ping_handler_(*this)
{
}

bool ProxyServer::start()
{
    constexpr auto config_path = "endgate.toml";

    const auto version = getVersion();
    logger_.info("Starting {} {}...", version.getName(), version.getVersion());

    auto configuration = ProxyConfig::read(config_path);
    if (!configuration) {
        logger_.error("Unable to read/load/save your endgate.toml. The server will stop. {}", configuration.error());
        return false;
    }
    configuration_ = std::move(*configuration);
    if (!configuration_.validate()) {
        logger_.error("Your configuration is invalid. Endgate will not start up until the errors are resolved.");
        return false;
    }

    ip_attempt_limiter_ = Ratelimiters::createWithMilliseconds<asio::ip::address>(configuration_.getLoginRatelimit());

    for (const auto &[name, address] : configuration_.getServers()) {
        if (auto info = ServerInfo::parse(name, address)) {
            servers_.try_emplace(name, std::move(*info));
        }
    }

    if (configuration_.isOnlineMode()) {
        keys_ = std::make_shared<MinecraftServiceKeyManager>(
            createHttpClient(), std::format("{}/{}", version.getName(), version.getVersion()),
            std::format("{}{}", MinecraftServiceKeyManager::DISCOVERY_URI, MAXIMUM_VERSION.getName()));
        keys_->start();
    }

    cm_.logChannelInformation();

    const auto &bind = configuration_.getBind();
    const auto colon = bind.rfind(':');
    if (colon == std::string::npos) {
        logger_.error("'bind' option does not specify a valid IP address.");
        return false;
    }

    asio::error_code ec;
    const auto address = asio::ip::make_address(bind.substr(0, colon), ec);
    const auto digits = bind.substr(colon + 1);
    std::uint16_t port{};
    if (ec || std::from_chars(digits.data(), digits.data() + digits.size(), port).ec != std::errc{}) {
        logger_.error("'bind' option does not specify a valid IP address.");
        return false;
    }

    if (!cm_.bind(address, port)) {
        return false;
    }

    cm_.start();
    return true;
}

void ProxyServer::shutdown([[maybe_unused]] bool explicit_exit)
{
    bool in_progress = false;
    if (!shutdown_in_progress_.compare_exchange_strong(in_progress, true)) {
        return;
    }

    logger_.info("Stopping the proxy...");
    if (keys_) {
        keys_->shutdown();
    }
    cm_.shutdown();
    shutdown_.store(true);
}

void ProxyServer::awaitProxyShutdown()
{
    cm_.awaitTermination();
}

HttpClient ProxyServer::createHttpClient()
{
    return cm_.createHttpClient();
}

bool ProxyServer::isShutdown() const
{
    return shutdown_.load();
}

bool ProxyServer::isShuttingDown() const
{
    return shutdown_in_progress_.load();
}

ProxyVersion ProxyServer::getVersion()
{
    return {"Endgate", "EndstoneMC", ENDGATE_VERSION};
}

Console &ProxyServer::getConsoleCommandSource()
{
    return console_;
}

const ServerListPingHandler &ProxyServer::getServerListPingHandler() const
{
    return ping_handler_;
}

const ProxyConfig &ProxyServer::getConfiguration() const
{
    return configuration_;
}

BackendServer *ProxyServer::getServer(std::string_view name)
{
    const auto server = servers_.find(name);
    return server == servers_.end() ? nullptr : &server->second;
}

Ratelimiter<asio::ip::address> &ProxyServer::getIpAttemptLimiter() const
{
    return *ip_attempt_limiter_;
}

}  // namespace endgate
