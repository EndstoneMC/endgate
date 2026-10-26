#include "endgate/config.h"

#include <format>
#include <fstream>
#include <string_view>
#include <system_error>
#include <variant>

#include <asio/ip/address.hpp>
#include <glaze/toml.hpp>
#include <spdlog/logger.h>

#include "endgate/default_config.h"
#include "endgate/logging/manager.h"

namespace endgate {

namespace {
struct Advanced {
    int compression_level{};

    struct glaze {
        using T = Advanced;
        static constexpr auto value = glz::object("compression-level", &T::compression_level);
    };
};

struct NetherNet {
    bool enabled{};
    std::vector<std::string> public_addresses;
    std::string tls_certificate_chain;
    std::string tls_private_key;
    std::string identity_key;
    std::string identity_domain;

    struct glaze {
        using T = NetherNet;
        static constexpr auto value =
            glz::object("enabled", &T::enabled, "public-addresses", &T::public_addresses, "tls-certificate-chain",
                        &T::tls_certificate_chain, "tls-private-key", &T::tls_private_key, "identity-key",
                        &T::identity_key, "identity-domain", &T::identity_domain);
    };
};

struct Document {
    std::string config_version;
    std::string bind;
    std::string motd;
    int show_max_players{};
    bool online_mode{};
    std::map<std::string, std::variant<std::string, std::vector<std::string>>> servers;
    NetherNet nethernet;
    Advanced advanced;

    struct glaze {
        using T = Document;
        static constexpr auto value =
            glz::object("config-version", &T::config_version, "bind", &T::bind, "motd", &T::motd, "show-max-players",
                        &T::show_max_players, "online-mode", &T::online_mode, "servers", &T::servers, "nethernet",
                        &T::nethernet, "advanced", &T::advanced);
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
    for (const auto &[name, entry] : document.servers) {
        if (const auto *address = std::get_if<std::string>(&entry)) {
            config.servers_.emplace(name, *address);
        }
        else if (name == "try") {
            config.attempt_connection_order_ = std::get<std::vector<std::string>>(entry);
        }
    }
    config.compression_level_ = document.advanced.compression_level;
    config.nethernet_enabled_ = document.nethernet.enabled;
    config.nethernet_public_addresses_ = document.nethernet.public_addresses;
    config.nethernet_tls_certificate_chain_ = document.nethernet.tls_certificate_chain;
    config.nethernet_tls_private_key_ = document.nethernet.tls_private_key;
    config.nethernet_identity_key_ = document.nethernet.identity_key;
    config.nethernet_identity_domain_ = document.nethernet.identity_domain;
    return config;
}

bool ProxyConfig::validate() const
{
    static spdlog::logger &logger = LogManager::getLogger();

    bool valid = true;
    if (compression_level_ < -1 || compression_level_ > 9) {
        logger.error("Invalid compression level {}", compression_level_);
        valid = false;
    }
    else if (compression_level_ == 0) {
        logger.warn("ALL packets going through the proxy will be uncompressed. This will increase bandwidth usage.");
    }

    if (nethernet_enabled_) {
        for (const auto &address : nethernet_public_addresses_) {
            asio::error_code ec;
            asio::ip::make_address(address, ec);
            if (ec) {
                logger.error("Invalid NetherNet public address {}", address);
                valid = false;
            }
        }
        if (nethernet_tls_certificate_chain_.empty() != nethernet_tls_private_key_.empty()) {
            logger.error("NetherNet over HTTPS needs both tls-certificate-chain and tls-private-key");
            valid = false;
        }
        if (nethernet_identity_key_.empty()) {
            logger.error("NetherNet needs an identity-key path");
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

const std::map<std::string, std::string> &ProxyConfig::getServers() const
{
    return servers_;
}

const std::vector<std::string> &ProxyConfig::getAttemptConnectionOrder() const
{
    return attempt_connection_order_;
}

int ProxyConfig::getCompressionLevel() const
{
    return compression_level_;
}

bool ProxyConfig::isNetherNetEnabled() const
{
    return nethernet_enabled_;
}

const std::vector<std::string> &ProxyConfig::getNetherNetPublicAddresses() const
{
    return nethernet_public_addresses_;
}

const std::filesystem::path &ProxyConfig::getNetherNetTlsCertificateChain() const
{
    return nethernet_tls_certificate_chain_;
}

const std::filesystem::path &ProxyConfig::getNetherNetTlsPrivateKey() const
{
    return nethernet_tls_private_key_;
}

const std::filesystem::path &ProxyConfig::getNetherNetIdentityKey() const
{
    return nethernet_identity_key_;
}

const std::string &ProxyConfig::getNetherNetIdentityDomain() const
{
    return nethernet_identity_domain_;
}

}  // namespace endgate
