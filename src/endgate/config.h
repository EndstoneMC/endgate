#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <spdlog/logger.h>

namespace endgate {

/**
 * The proxy's configuration, as read from endgate.toml.
 *
 * @see com.velocitypowered.api.proxy.config.ProxyConfig
 * @see com.velocitypowered.proxy.config.VelocityConfiguration
 */
class ProxyConfig {
public:
    static std::expected<ProxyConfig, std::string> read(const std::filesystem::path &path);

    [[nodiscard]] bool validate() const;

    [[nodiscard]] const std::string &getBind() const;
    [[nodiscard]] const std::string &getMotd() const;
    [[nodiscard]] int getShowMaxPlayers() const;
    [[nodiscard]] bool isOnlineMode() const;
    [[nodiscard]] bool isOnlineModeKickExistingPlayers() const;
    [[nodiscard]] const std::map<std::string, std::string> &getServers() const;
    [[nodiscard]] const std::vector<std::string> &getAttemptConnectionOrder() const;
    [[nodiscard]] const std::map<std::string, std::vector<std::string>> &getForcedHosts() const;
    [[nodiscard]] int getCompressionThreshold() const;
    [[nodiscard]] int getCompressionLevel() const;
    [[nodiscard]] int getLoginRatelimit() const;
    [[nodiscard]] int getConnectTimeout() const;
    [[nodiscard]] int getReadTimeout() const;
    [[nodiscard]] bool isLogCommandExecutions() const;
    [[nodiscard]] bool isLogPlayerConnections() const;
    [[nodiscard]] bool isFailoverOnUnexpectedServerDisconnect() const;
    [[nodiscard]] bool isQueryEnabled() const;
    [[nodiscard]] int getQueryPort() const;
    [[nodiscard]] const std::string &getQueryMap() const;
    [[nodiscard]] bool shouldQueryShowPlugins() const;
    [[nodiscard]] bool isPacketDecodeLogging() const;
    [[nodiscard]] bool isLogServerBackpressure() const;
    [[nodiscard]] const std::filesystem::path &getTlsCertificate() const;
    [[nodiscard]] const std::filesystem::path &getTlsCertificateKey() const;

private:
    [[nodiscard]] static spdlog::logger &logger();

    std::string bind_;
    std::string motd_;
    int show_max_players_ = 0;
    bool online_mode_ = false;
    bool kick_existing_players_ = false;
    std::map<std::string, std::string> servers_;
    std::vector<std::string> attempt_connection_order_;
    std::map<std::string, std::vector<std::string>> forced_hosts_;
    int compression_threshold_ = 0;
    int compression_level_ = 0;
    int login_ratelimit_ = 0;
    int connection_timeout_ = 0;
    int read_timeout_ = 0;
    bool log_command_executions_ = false;
    bool log_player_connections_ = false;
    bool failover_on_unexpected_server_disconnect_ = false;
    bool query_enabled_ = false;
    int query_port_ = 0;
    std::string query_map_;
    bool query_show_plugins_ = false;
    bool packet_decode_logging_ = false;
    bool log_server_backpressure_ = false;
    std::filesystem::path tls_certificate_;
    std::filesystem::path tls_certificate_key_;
};

}  // namespace endgate
