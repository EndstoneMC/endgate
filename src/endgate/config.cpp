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

struct Document {
    std::string config_version;
    std::string bind;
    std::vector<std::string> public_addresses;
    std::string tls_certificate;
    std::string tls_certificate_key;
    std::string motd;
    int show_max_players{};
    bool online_mode{};
    std::map<std::string, std::variant<std::string, std::vector<std::string>>> servers;
    Advanced advanced;

    struct glaze {
        using T = Document;
        static constexpr auto value =
            glz::object("config-version", &T::config_version, "bind", &T::bind, "public-addresses",
                        &T::public_addresses, "tls-certificate", &T::tls_certificate, "tls-certificate-key",
                        &T::tls_certificate_key, "motd", &T::motd, "show-max-players", &T::show_max_players,
                        "online-mode", &T::online_mode, "servers", &T::servers, "advanced", &T::advanced);
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
    config.public_addresses_ = document.public_addresses;
    config.tls_certificate_ = document.tls_certificate;
    config.tls_certificate_key_ = document.tls_certificate_key;
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

    for (const auto &address : public_addresses_) {
        asio::error_code ec;
        asio::ip::make_address(address, ec);
        if (ec) {
            logger.error("Invalid public address {}", address);
            valid = false;
        }
    }
    if (tls_certificate_.empty() != tls_certificate_key_.empty()) {
        logger.error("HTTPS needs both tls-certificate and tls-certificate-key");
        valid = false;
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

const std::vector<std::string> &ProxyConfig::getPublicAddresses() const
{
    return public_addresses_;
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
