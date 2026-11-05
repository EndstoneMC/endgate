#include "endgate/config.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <glaze/toml.hpp>
#include <spdlog/logger.h>

#include "endgate/backend.h"
#include "endgate/default_config.h"
#include "endgate/logging/manager.h"

namespace endgate {

namespace {
struct Advanced {
    int compression_threshold = 0;
    int compression_level = 0;
    int login_ratelimit = 0;
    int connection_timeout = 0;
    int read_timeout = 0;
    bool announce_proxy_commands = true;
    bool log_command_executions = false;
    int command_rate_limit = 0;
    bool forward_commands_if_rate_limited = true;
    int kick_after_rate_limited_commands = 0;
    bool log_player_connections = false;
    bool failover_on_unexpected_server_disconnect = false;
    bool show_ping_requests = false;

    struct glaze {
        using T = Advanced;
        static constexpr auto value = glz::object(                                                     //
            "compression-threshold", &T::compression_threshold,                                        //
            "compression-level", &T::compression_level,                                                //
            "login-ratelimit", &T::login_ratelimit,                                                    //
            "connection-timeout", &T::connection_timeout,                                              //
            "read-timeout", &T::read_timeout,                                                          //
            "announce-proxy-commands", &T::announce_proxy_commands,                                    //
            "log-command-executions", &T::log_command_executions,                                      //
            "command-rate-limit", &T::command_rate_limit,                                              //
            "forward-commands-if-rate-limited", &T::forward_commands_if_rate_limited,                  //
            "kick-after-rate-limited-commands", &T::kick_after_rate_limited_commands,                  //
            "log-player-connections", &T::log_player_connections,                                      //
            "failover-on-unexpected-server-disconnect", &T::failover_on_unexpected_server_disconnect,  //
            "show-ping-requests", &T::show_ping_requests);
    };
};

struct Query {
    bool enabled = false;
    int port = 0;
    std::string map;
    bool show_plugins = false;

    struct glaze {
        using T = Query;
        static constexpr auto value = glz::object(  //
            "enabled", &T::enabled,                 //
            "port", &T::port,                       //
            "map", &T::map,                         //
            "show-plugins", &T::show_plugins);
    };
};

struct Metrics {
    bool enabled = true;

    struct glaze {
        using T = Metrics;
        static constexpr auto value = glz::object("enabled", &T::enabled);
    };
};

struct Debug {
    bool packet_decode_logging = false;
    bool log_server_backpressure = false;

    struct glaze {
        using T = Debug;
        static constexpr auto value = glz::object(               //
            "packet-decode-logging", &T::packet_decode_logging,  //
            "log-server-backpressure", &T::log_server_backpressure);
    };
};

struct Document {
    std::string config_version;
    std::string bind;
    std::string tls_certificate;
    std::string tls_certificate_key;
    std::string motd;
    int show_max_players = 0;
    bool online_mode = false;
    bool kick_existing_players = false;
    std::string player_info_forwarding_mode;
    std::string ping_passthrough;
    bool enable_player_address_logging = false;
    std::map<std::string, std::variant<std::string, std::vector<std::string>>> servers;
    std::optional<std::map<std::string, std::variant<std::string, std::vector<std::string>>>> forced_hosts;
    Advanced advanced;
    Query query;
    Metrics metrics;
    Debug debug;

    struct glaze {
        using T = Document;
        static constexpr auto value = glz::object(                               //
            "config-version", &T::config_version,                                //
            "bind", &T::bind,                                                    //
            "tls-certificate", &T::tls_certificate,                              //
            "tls-certificate-key", &T::tls_certificate_key,                      //
            "motd", &T::motd,                                                    //
            "show-max-players", &T::show_max_players,                            //
            "online-mode", &T::online_mode,                                      //
            "kick-existing-players", &T::kick_existing_players,                  //
            "player-info-forwarding-mode", &T::player_info_forwarding_mode,      //
            "ping-passthrough", &T::ping_passthrough,                            //
            "enable-player-address-logging", &T::enable_player_address_logging,  //
            "servers", &T::servers,                                              //
            "forced-hosts", &T::forced_hosts,                                    //
            "advanced", &T::advanced,                                            //
            "query", &T::query,                                                  //
            "metrics", &T::metrics,                                              //
            "debug", &T::debug);
    };
};
}  // namespace

std::expected<ProxyConfig, std::string> ProxyConfig::read(const std::filesystem::path &path)
{
    if (!std::filesystem::exists(path)) {
        std::ofstream out(path);
        if (!out) {
            return std::unexpected(std::format("unable to create {}", path.string()));
        }
        out << default_config;
    }

    Document defaults;
    if (const auto ec = glz::read_toml(defaults, default_config)) {
        return std::unexpected(glz::format_error(ec, default_config));
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected(std::format("unable to read {}", path.string()));
    }

    Document document = defaults;
    document.servers.clear();
    document.forced_hosts.reset();
    std::string buffer;
    if (size > 0) {
        if (const auto ec = glz::read_file_toml(document, path.string(), buffer)) {
            return std::unexpected(glz::format_error(ec, buffer));
        }
    }
    if (document.servers.empty()) {
        document.servers = defaults.servers;
    }
    if (!document.forced_hosts) {
        document.forced_hosts = defaults.forced_hosts;
    }

    std::string passthrough = document.ping_passthrough;
    std::ranges::transform(passthrough, passthrough.begin(), [](unsigned char c) { return std::toupper(c); });
    constexpr std::pair<std::string_view, PingPassthroughMode> modes[] = {
        {"DISABLED", PingPassthroughMode::Disabled},
        {"DESCRIPTION", PingPassthroughMode::Description},
        {"ALL", PingPassthroughMode::All}};
    const auto *const mode =
        std::ranges::find(modes, passthrough, &std::pair<std::string_view, PingPassthroughMode>::first);
    if (mode == std::end(modes)) {
        return std::unexpected(std::format("invalid ping-passthrough '{}'", document.ping_passthrough));
    }

    std::string forwarding = document.player_info_forwarding_mode;
    std::ranges::transform(forwarding, forwarding.begin(), [](unsigned char c) { return std::toupper(c); });
    constexpr std::pair<std::string_view, PlayerInfoForwarding> forwarding_modes[] = {
        {"NONE", PlayerInfoForwarding::None}, {"MODERN", PlayerInfoForwarding::Modern}};
    const auto *const forwarding_mode =
        std::ranges::find(forwarding_modes, forwarding, &std::pair<std::string_view, PlayerInfoForwarding>::first);
    if (forwarding_mode == std::end(forwarding_modes)) {
        return std::unexpected(
            std::format("invalid player-info-forwarding-mode '{}'", document.player_info_forwarding_mode));
    }

    ProxyConfig config;
    config.player_info_forwarding_mode_ = forwarding_mode->second;
    config.ping_passthrough_ = mode->second;
    config.show_ping_requests_ = document.advanced.show_ping_requests;
    config.bind_ = document.bind;
    config.motd_ = document.motd;
    config.show_max_players_ = document.show_max_players;
    config.online_mode_ = document.online_mode;
    config.kick_existing_players_ = document.kick_existing_players;
    config.enable_player_address_logging_ = document.enable_player_address_logging;
    for (const auto &[name, entry] : document.servers) {
        if (const auto *address = std::get_if<std::string>(&entry)) {
            config.servers_.emplace(name, *address);
        }
        else if (name == "try") {
            config.attempt_connection_order_ = std::get<std::vector<std::string>>(entry);
        }
    }
    for (const auto &[host, entry] : *document.forced_hosts) {
        std::string lowered = host;
        std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return std::tolower(c); });
        if (const auto *server = std::get_if<std::string>(&entry)) {
            config.forced_hosts_[lowered] = {*server};
        }
        else {
            config.forced_hosts_[lowered] = std::get<std::vector<std::string>>(entry);
        }
    }
    config.compression_threshold_ = document.advanced.compression_threshold;
    config.compression_level_ = document.advanced.compression_level;
    config.login_ratelimit_ = document.advanced.login_ratelimit;
    config.connection_timeout_ = document.advanced.connection_timeout;
    config.read_timeout_ = document.advanced.read_timeout;
    config.announce_proxy_commands_ = document.advanced.announce_proxy_commands;
    config.log_command_executions_ = document.advanced.log_command_executions;
    config.command_rate_limit_ = document.advanced.command_rate_limit;
    config.forward_commands_if_rate_limited_ = document.advanced.forward_commands_if_rate_limited;
    config.kick_after_rate_limited_commands_ = document.advanced.kick_after_rate_limited_commands;
    config.log_player_connections_ = document.advanced.log_player_connections;
    config.failover_on_unexpected_server_disconnect_ = document.advanced.failover_on_unexpected_server_disconnect;
    config.query_enabled_ = document.query.enabled;
    config.query_port_ = document.query.port;
    config.query_map_ = document.query.map;
    config.query_show_plugins_ = document.query.show_plugins;
    config.metrics_enabled_ = document.metrics.enabled;
    config.packet_decode_logging_ = document.debug.packet_decode_logging;
    config.log_server_backpressure_ = document.debug.log_server_backpressure;
    config.tls_certificate_ = document.tls_certificate;
    config.tls_certificate_key_ = document.tls_certificate_key;
    return config;
}

spdlog::logger &ProxyConfig::logger()
{
    static spdlog::logger &logger = LogManager::getLogger("ProxyConfig");
    return logger;
}

bool ProxyConfig::validate() const
{

    bool valid = true;
    if (bind_.empty()) {
        logger().error("'bind' option is empty.");
        valid = false;
    }

    if (!online_mode_) {
        logger().warn("The proxy is running in offline mode! This is a security risk and you will NOT receive any "
                      "support!");
    }

    switch (player_info_forwarding_mode_) {
    case PlayerInfoForwarding::None:
        logger().warn("Player info forwarding is disabled! All players will appear to be connecting from the proxy "
                      "with a self-signed login.");
        break;
    case PlayerInfoForwarding::Modern:
        if (!online_mode_) {
            logger().error("Modern player info forwarding needs online-mode, since the proxy would vouch for "
                           "players it never authenticated.");
            valid = false;
        }
        break;
    }

    if (servers_.empty()) {
        logger().warn("You don't have any servers configured.");
    }

    if (compression_threshold_ < -1 ||
        std::cmp_greater(compression_threshold_, std::numeric_limits<std::uint16_t>::max())) {
        logger().error("Invalid compression threshold {}", compression_threshold_);
        valid = false;
    }
    else if (compression_threshold_ == 0) {
        logger().warn("ALL packets going through the proxy will be compressed. This will compromise throughput and "
                      "increase CPU usage!");
    }

    if (compression_level_ < -1 || compression_level_ > 9) {
        logger().error("Invalid compression level {}", compression_level_);
        valid = false;
    }
    else if (compression_level_ == 0) {
        logger().warn("ALL packets going through the proxy will be uncompressed. This will increase bandwidth usage.");
    }

    if (login_ratelimit_ < 0) {
        logger().error("Invalid login ratelimit {}ms", login_ratelimit_);
        valid = false;
    }

    if (command_rate_limit_ < 0) {
        logger().error("Invalid command rate limit {}", command_rate_limit_);
        valid = false;
    }

    if (tls_certificate_.empty() != tls_certificate_key_.empty()) {
        logger().error("HTTPS needs both tls-certificate and tls-certificate-key");
        valid = false;
    }

    for (const auto &[name, address] : servers_) {
        if (!ServerInfo::parse(name, address)) {
            logger().error("Server {} does not have a valid IP address.", name);
            valid = false;
        }
    }
    for (const auto &name : attempt_connection_order_) {
        if (!servers_.contains(name)) {
            logger().error("Fallback server {} is not registered in your configuration!", name);
            valid = false;
        }
    }
    for (const auto &[host, servers] : forced_hosts_) {
        if (servers.empty()) {
            logger().error("Forced host '{}' does not contain any servers", host);
            valid = false;
            continue;
        }
        for (const auto &name : servers) {
            if (!servers_.contains(name)) {
                logger().error("Server '{}' for forced host '{}' does not exist", name, host);
                valid = false;
            }
        }
    }
    return valid;
}

const std::string &ProxyConfig::getBind() const
{
    return bind_;
}

const std::string &ProxyConfig::getMotd() const
{
    return motd_;
}

int ProxyConfig::getShowMaxPlayers() const
{
    return show_max_players_;
}

bool ProxyConfig::isOnlineMode() const
{
    return online_mode_;
}

bool ProxyConfig::isOnlineModeKickExistingPlayers() const
{
    return kick_existing_players_;
}

PlayerInfoForwarding ProxyConfig::getPlayerInfoForwardingMode() const
{
    return player_info_forwarding_mode_;
}

PingPassthroughMode ProxyConfig::getPingPassthrough() const
{
    return ping_passthrough_;
}

bool ProxyConfig::isShowPingRequests() const
{
    return show_ping_requests_;
}

bool ProxyConfig::isPlayerAddressLoggingEnabled() const
{
    return enable_player_address_logging_;
}

const std::map<std::string, std::string> &ProxyConfig::getServers() const
{
    return servers_;
}

const std::vector<std::string> &ProxyConfig::getAttemptConnectionOrder() const
{
    return attempt_connection_order_;
}

const std::map<std::string, std::vector<std::string>> &ProxyConfig::getForcedHosts() const
{
    return forced_hosts_;
}

int ProxyConfig::getCompressionThreshold() const
{
    return compression_threshold_;
}

int ProxyConfig::getCompressionLevel() const
{
    return compression_level_;
}

int ProxyConfig::getLoginRatelimit() const
{
    return login_ratelimit_;
}

int ProxyConfig::getConnectTimeout() const
{
    return connection_timeout_;
}

int ProxyConfig::getReadTimeout() const
{
    return read_timeout_;
}

bool ProxyConfig::isLogCommandExecutions() const
{
    return log_command_executions_;
}

int ProxyConfig::getCommandRatelimit() const
{
    return command_rate_limit_;
}

bool ProxyConfig::isForwardCommandsIfRateLimited() const
{
    return forward_commands_if_rate_limited_;
}

int ProxyConfig::getKickAfterRateLimitedCommands() const
{
    return kick_after_rate_limited_commands_;
}

bool ProxyConfig::isKickOnCommandRateLimit() const
{
    return kick_after_rate_limited_commands_ > 0;
}

bool ProxyConfig::isAnnounceProxyCommands() const
{
    return announce_proxy_commands_;
}

bool ProxyConfig::isLogPlayerConnections() const
{
    return log_player_connections_;
}

bool ProxyConfig::isFailoverOnUnexpectedServerDisconnect() const
{
    return failover_on_unexpected_server_disconnect_;
}

bool ProxyConfig::isQueryEnabled() const
{
    return query_enabled_;
}

int ProxyConfig::getQueryPort() const
{
    return query_port_;
}

const std::string &ProxyConfig::getQueryMap() const
{
    return query_map_;
}

bool ProxyConfig::shouldQueryShowPlugins() const
{
    return query_show_plugins_;
}

bool ProxyConfig::isMetricsEnabled() const
{
    return metrics_enabled_;
}

bool ProxyConfig::isPacketDecodeLogging() const
{
    return packet_decode_logging_;
}

bool ProxyConfig::isLogServerBackpressure() const
{
    return log_server_backpressure_;
}

const std::filesystem::path &ProxyConfig::getTlsCertificate() const
{
    return tls_certificate_;
}

const std::filesystem::path &ProxyConfig::getTlsCertificateKey() const
{
    return tls_certificate_key_;
}

}  // namespace endgate
