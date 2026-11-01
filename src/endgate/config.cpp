#include "endgate/config.h"

#include <format>
#include <fstream>
#include <limits>
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
    bool log_command_executions = false;
    bool log_player_connections = false;
    bool failover_on_unexpected_server_disconnect = false;

    struct glaze {
        using T = Advanced;
        static constexpr auto value =
            glz::object("compression-threshold", &T::compression_threshold, "compression-level", &T::compression_level,
                        "login-ratelimit", &T::login_ratelimit, "connection-timeout", &T::connection_timeout,
                        "read-timeout", &T::read_timeout, "log-command-executions", &T::log_command_executions,
                        "log-player-connections", &T::log_player_connections,
                        "failover-on-unexpected-server-disconnect", &T::failover_on_unexpected_server_disconnect);
    };
};

struct Debug {
    bool packet_decode_logging = false;
    bool log_server_backpressure = false;

    struct glaze {
        using T = Debug;
        static constexpr auto value = glz::object("packet-decode-logging", &T::packet_decode_logging,
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
    std::map<std::string, std::variant<std::string, std::vector<std::string>>> servers;
    Advanced advanced;
    Debug debug;

    struct glaze {
        using T = Document;
        static constexpr auto value = glz::object(
            "config-version", &T::config_version, "bind", &T::bind, "tls-certificate", &T::tls_certificate,
            "tls-certificate-key", &T::tls_certificate_key, "motd", &T::motd, "show-max-players", &T::show_max_players,
            "online-mode", &T::online_mode, "kick-existing-players", &T::kick_existing_players, "servers", &T::servers,
            "advanced", &T::advanced, "debug", &T::debug);
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
    std::string buffer;
    if (size > 0) {
        if (const auto ec = glz::read_file_toml(document, path.string(), buffer)) {
            return std::unexpected(glz::format_error(ec, buffer));
        }
    }
    if (document.servers.empty()) {
        document.servers = defaults.servers;
    }

    ProxyConfig config;
    config.bind_ = document.bind;
    config.motd_ = document.motd;
    config.show_max_players_ = document.show_max_players;
    config.online_mode_ = document.online_mode;
    config.kick_existing_players_ = document.kick_existing_players;
    for (const auto &[name, entry] : document.servers) {
        if (const auto *address = std::get_if<std::string>(&entry)) {
            config.servers_.emplace(name, *address);
        }
        else if (name == "try") {
            config.attempt_connection_order_ = std::get<std::vector<std::string>>(entry);
        }
    }
    config.compression_threshold_ = document.advanced.compression_threshold;
    config.compression_level_ = document.advanced.compression_level;
    config.login_ratelimit_ = document.advanced.login_ratelimit;
    config.connection_timeout_ = document.advanced.connection_timeout;
    config.read_timeout_ = document.advanced.read_timeout;
    config.log_command_executions_ = document.advanced.log_command_executions;
    config.log_player_connections_ = document.advanced.log_player_connections;
    config.failover_on_unexpected_server_disconnect_ = document.advanced.failover_on_unexpected_server_disconnect;
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

const std::map<std::string, std::string> &ProxyConfig::getServers() const
{
    return servers_;
}

const std::vector<std::string> &ProxyConfig::getAttemptConnectionOrder() const
{
    return attempt_connection_order_;
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

bool ProxyConfig::isLogPlayerConnections() const
{
    return log_player_connections_;
}

bool ProxyConfig::isFailoverOnUnexpectedServerDisconnect() const
{
    return failover_on_unexpected_server_disconnect_;
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
