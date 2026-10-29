#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

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
    [[nodiscard]] const std::map<std::string, std::string> &getServers() const;
    [[nodiscard]] const std::vector<std::string> &getAttemptConnectionOrder() const;
    [[nodiscard]] int getCompressionLevel() const;
    [[nodiscard]] bool isLogCommandExecutions() const;
    [[nodiscard]] bool isLogPlayerConnections() const;
    [[nodiscard]] bool isPacketDecodeLogging() const;
    [[nodiscard]] bool isLogServerBackpressure() const;
    [[nodiscard]] const std::filesystem::path &getTlsCertificate() const;
    [[nodiscard]] const std::filesystem::path &getTlsCertificateKey() const;

private:
    std::string bind_;
    std::string motd_;
    int show_max_players_{};
    bool online_mode_{};
    std::map<std::string, std::string> servers_;
    std::vector<std::string> attempt_connection_order_;
    int compression_level_{};
    bool log_command_executions_{};
    bool log_player_connections_{};
    bool packet_decode_logging_{};
    bool log_server_backpressure_{};
    std::filesystem::path tls_certificate_;
    std::filesystem::path tls_certificate_key_;
};

}  // namespace endgate
