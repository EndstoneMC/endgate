#include "endgate/server.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <format>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/address.hpp>

#include "endgate/connection/backend.h"
#include "endgate/connection/player.h"
#include "endgate/logging/manager.h"
#include "endgate/network/version.h"

namespace endgate {

namespace {
constexpr auto CONFIG_PATH = "endgate.toml";
}  // namespace

ProxyServer::ProxyServer() : logger_(LogManager::getLogger(*this)), cm_(*this), console_(*this), ping_handler_(*this)
{
    configuration_ = configurations_.emplace_back(std::make_unique<ProxyConfig>()).get();
}

bool ProxyServer::start()
{
    const auto version = getVersion();
    logger_.info("Starting {} {}...", version.getName(), version.getVersion());

    auto configuration = ProxyConfig::read(CONFIG_PATH);
    if (!configuration) {
        logger_.error("Unable to read/load/save your endgate.toml. The server will stop. {}", configuration.error());
        return false;
    }
    if (!configuration->validate()) {
        logger_.error("Your configuration is invalid. Endgate will not start up until the errors are resolved.");
        return false;
    }
    configuration_ = configurations_.emplace_back(std::make_unique<ProxyConfig>(std::move(*configuration))).get();
    const auto &current = getConfiguration();

    ip_attempt_limiter_ =
        ip_attempt_limiters_
            .emplace_back(Ratelimiters::createWithMilliseconds<asio::ip::address>(current.getLoginRatelimit()))
            .get();

    for (const auto &[name, address] : current.getServers()) {
        if (auto info = ServerInfo::parse(name, address)) {
            servers_.try_emplace(name, std::make_unique<BackendServer>(std::move(*info)));
        }
    }

    if (current.isOnlineMode()) {
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

    const auto bind = parseBind(current.getBind());
    if (!bind) {
        logger_.error("'bind' option does not specify a valid IP address.");
        return false;
    }

    if (!cm_.bind(bind->address(), bind->port())) {
        return false;
    }

    cm_.start();
    return true;
}

std::optional<asio::ip::udp::endpoint> ProxyServer::parseBind(const std::string &bind)
{
    const auto colon = bind.rfind(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }

    asio::error_code ec;
    const auto address = asio::ip::make_address(bind.substr(0, colon), ec);
    const auto digits = bind.substr(colon + 1);
    std::uint16_t port{};
    if (ec || std::from_chars(digits.data(), digits.data() + digits.size(), port).ec != std::errc{}) {
        return std::nullopt;
    }
    return asio::ip::udp::endpoint{address, port};
}

bool ProxyServer::reloadConfiguration()
{
    const std::scoped_lock reload_lock(reload_mutex_);
    auto read = ProxyConfig::read(CONFIG_PATH);
    if (!read) {
        logger_.error("Unable to reload configuration: {}", read.error());
        return false;
    }
    auto new_configuration = std::make_unique<ProxyConfig>(std::move(*read));
    if (!new_configuration->validate()) {
        return false;
    }

    std::vector<const BackendServer *> replaced;
    {
        const std::scoped_lock lock(servers_mutex_);
        for (const auto &[name, address] : new_configuration->getServers()) {
            auto new_info = ServerInfo::parse(name, address);
            const auto existing = servers_.find(name);
            if (existing == servers_.end()) {
                servers_.try_emplace(name, std::make_unique<BackendServer>(std::move(*new_info)));
            }
            else if (existing->second->getServerInfo() != *new_info) {
                replaced.push_back(existing->second.get());
                retired_servers_.push_back(std::move(existing->second));
                existing->second = std::make_unique<BackendServer>(std::move(*new_info));
            }
        }
    }

    if (!replaced.empty()) {
        const auto players = getAllPlayers();
        std::latch moved(static_cast<std::ptrdiff_t>(players.size()));
        for (const auto &player : players) {
            asio::co_spawn(player->getConnection().eventLoop().getExecutor(), evacuate(player, replaced, &moved),
                           asio::detached);
        }
        moved.wait();
    }

    const auto &current = getConfiguration();
    if (current.getBind() != new_configuration->getBind()) {
        logger_.warn("Changing 'bind' needs a restart. Still listening on {}.", current.getBind());
    }

    const auto query_port_changed = new_configuration->getQueryPort() != current.getQueryPort();
    const auto query_already_enabled = current.isQueryEnabled();
    const auto query_enabled = new_configuration->isQueryEnabled();
    if (query_already_enabled && (!query_enabled || query_port_changed)) {
        cm_.queryClose();
    }
    if (query_enabled && (!query_already_enabled || query_port_changed)) {
        if (const auto bind = parseBind(current.getBind())) {
            cm_.queryBind(bind->address(), static_cast<std::uint16_t>(new_configuration->getQueryPort()));
        }
    }

    ip_attempt_limiter_ = ip_attempt_limiters_
                              .emplace_back(Ratelimiters::createWithMilliseconds<asio::ip::address>(
                                  new_configuration->getLoginRatelimit()))
                              .get();
    configuration_ = configurations_.emplace_back(std::move(new_configuration)).get();
    return true;
}

Awaitable<void> ProxyServer::evacuate(std::shared_ptr<Player> player, std::vector<const BackendServer *> replaced,
                                      std::latch *moved)
{
    constexpr auto message = "Your server has been changed, but we could not move you to any fallback servers.";

    const auto connected = player->getConnectedServer();
    if (!connected || !std::ranges::contains(replaced, &connected->getServer())) {
        moved->count_down();
        co_return;
    }

    auto *const next = player->getNextServerToTry();
    if (next == nullptr) {
        player->disconnect(DisconnectFailReason::NoReason, message);
        moved->count_down();
        co_return;
    }

    bool success = false;
    try {
        success = co_await player->createConnectionRequest(*next).connectWithIndication();
    }
    catch (const std::exception &) {
        success = false;
    }
    if (!success) {
        player->disconnect(DisconnectFailReason::NoReason, message);
    }
    moved->count_down();
}

void ProxyServer::shutdown([[maybe_unused]] bool explicit_exit, const std::string &reason)
{
    bool in_progress = false;
    if (!shutdown_in_progress_.compare_exchange_strong(in_progress, true)) {
        return;
    }

    shutdown_thread_ = std::jthread([this, reason] {
        logger_.info("Shutting down the proxy...");

        const auto players = getAllPlayers();
        for (const auto &player : players) {
            player->disconnect(DisconnectFailReason::NoReason, reason);
        }

        std::unique_lock lock(connections_mutex_);
        const auto torn_down = connections_changed_.wait_for(lock, std::chrono::seconds{10}, [&players, this] {
            return std::ranges::none_of(players, [this](const auto &player) {
                const auto found = connections_by_uuid_.find(player->getUniqueId());
                return found != connections_by_uuid_.end() && found->second == player;
            });
        });
        lock.unlock();
        if (!torn_down) {
            logger_.error("Some players took over 10 seconds to disconnect.");
        }

        if (keys_) {
            keys_->shutdown();
        }
        cm_.shutdown();
        shutdown_.store(true);
    });
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
    return *configuration_.load();
}

BackendServer *ProxyServer::getServer(std::string_view name)
{
    const std::scoped_lock lock(servers_mutex_);
    const auto server = servers_.find(name);
    return server == servers_.end() ? nullptr : server->second.get();
}

std::vector<BackendServer *> ProxyServer::getAllServers()
{
    const std::scoped_lock lock(servers_mutex_);
    std::vector<BackendServer *> servers;
    servers.reserve(servers_.size());
    for (auto &[name, server] : servers_) {
        servers.push_back(server.get());
    }
    return servers;
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
    return *ip_attempt_limiter_.load();
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
    if (getConfiguration().isOnlineMode() && getConfiguration().isOnlineModeKickExistingPlayers()) {
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
        if (!getConfiguration().isOnlineModeKickExistingPlayers()) {
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
    connections_changed_.notify_all();
}

}  // namespace endgate
