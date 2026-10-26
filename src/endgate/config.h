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
    [[nodiscard]] const std::vector<std::string> &getNetherNetPublicAddresses() const;
    [[nodiscard]] const std::filesystem::path &getNetherNetTlsCertificateChain() const;
    [[nodiscard]] const std::filesystem::path &getNetherNetTlsPrivateKey() const;
    [[nodiscard]] const std::filesystem::path &getNetherNetIdentityKey() const;
    [[nodiscard]] const std::string &getNetherNetIdentityDomain() const;

private:
    std::string bind_;
    std::string motd_;
    int show_max_players_{};
    bool online_mode_{};
    std::map<std::string, std::string> servers_;
    std::vector<std::string> attempt_connection_order_;
    int compression_level_{};
    std::vector<std::string> nethernet_public_addresses_;
    std::filesystem::path nethernet_tls_certificate_chain_;
    std::filesystem::path nethernet_tls_private_key_;
    std::filesystem::path nethernet_identity_key_;
    std::string nethernet_identity_domain_;
};

}  // namespace endgate
