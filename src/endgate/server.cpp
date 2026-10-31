#include "endgate/server.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <utility>

#include <asio/ip/address.hpp>

#include "endgate/connection/player.h"
#include "endgate/logging/manager.h"
#include "endgate/network/version.h"

namespace endgate {

ProxyServer::ProxyServer() : logger_(LogManager::getLogger(*this)), cm_(*this), console_(*this), ping_handler_(*this) {}

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
        auto discovery_uri = std::format("{}{}", MinecraftServiceKeyManager::DISCOVERY_URI, MAXIMUM_VERSION.getName());
        if (const auto *const override_uri = std::getenv("ENDGATE_DISCOVERY_URI")) {
            logger_.warn("Minecraft services are discovered at {} instead of the real service.", override_uri);
            discovery_uri = override_uri;
        }
        keys_ = std::make_shared<MinecraftServiceKeyManager>(
            createHttpClient(), std::format("{}/{}", version.getName(), version.getVersion()),
            std::move(discovery_uri));
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

std::shared_ptr<Player> ProxyServer::getPlayer(std::string_view username)
{
    const std::scoped_lock lock(connections_mutex_);
    const auto player = connections_by_name_.find(std::string{username});
    return player == connections_by_name_.end() ? nullptr : player->second;
}

std::vector<std::shared_ptr<Player>> ProxyServer::getAllPlayers()
{
    const std::scoped_lock lock(connections_mutex_);
    std::vector<std::shared_ptr<Player>> players;
    players.reserve(connections_by_uuid_.size());
    for (const auto &[uuid, player] : connections_by_uuid_) {
        players.push_back(player);
    }
    return players;
}

Ratelimiter<asio::ip::address> &ProxyServer::getIpAttemptLimiter() const
{
    return *ip_attempt_limiter_;
}

bool ProxyServer::CaseInsensitiveLess::operator()(std::string_view left, std::string_view right) const
{
    const auto length = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < length; ++i) {
        const auto left_char = left[i] >= 'A' && left[i] <= 'Z' ? left[i] + ('a' - 'A') : left[i];
        const auto right_char = right[i] >= 'A' && right[i] <= 'Z' ? right[i] + ('a' - 'A') : right[i];
        if (left_char != right_char) {
            return left_char < right_char;
        }
    }
    return left.size() < right.size();
}

bool ProxyServer::canRegisterConnection(const Player &connection)
{
    if (configuration_.isOnlineMode() && configuration_.isOnlineModeKickExistingPlayers()) {
        return true;
    }
    const std::scoped_lock lock(connections_mutex_);
    return !(connections_by_name_.contains(connection.getUsername()) ||
             connections_by_uuid_.contains(connection.getUniqueId()));
}

bool ProxyServer::registerConnection(const std::shared_ptr<Player> &connection)
{
    std::shared_ptr<Player> existing;
    {
        const std::scoped_lock lock(connections_mutex_);
        if (!configuration_.isOnlineModeKickExistingPlayers()) {
            if (connections_by_name_.contains(connection->getUsername()) ||
                connections_by_uuid_.contains(connection->getUniqueId())) {
                return false;
            }
        }
        else if (const auto found = connections_by_uuid_.find(connection->getUniqueId());
                 found != connections_by_uuid_.end()) {
            existing = found->second;
        }
        connections_by_name_.insert_or_assign(connection->getUsername(), connection);
        connections_by_uuid_.insert_or_assign(connection->getUniqueId(), connection);
    }
    if (existing) {
        existing->disconnect(DisconnectFailReason::LoggedInOtherLocation);
    }
    return true;
}

void ProxyServer::unregisterConnection(const Player &connection)
{
    const std::scoped_lock lock(connections_mutex_);
    if (const auto by_name = connections_by_name_.find(connection.getUsername());
        by_name != connections_by_name_.end() && by_name->second.get() == &connection) {
        connections_by_name_.erase(by_name);
    }
    if (const auto by_uuid = connections_by_uuid_.find(connection.getUniqueId());
        by_uuid != connections_by_uuid_.end() && by_uuid->second.get() == &connection) {
        connections_by_uuid_.erase(by_uuid);
    }
}

}  // namespace endgate
