#include "endgate/config.h"

#include <format>
#include <fstream>
#include <string_view>
#include <system_error>
#include <variant>

#include <glaze/toml.hpp>

#include "endgate/default_config.h"

namespace endgate {

namespace {
struct Document {
    std::string config_version;
    std::string bind;
    std::string motd;
    int show_max_players{};
    bool online_mode{};
    std::map<std::string, std::variant<std::string, std::vector<std::string>>> servers;

    struct glaze {
        using T = Document;
        static constexpr auto value =
            glz::object("config-version", &T::config_version, "bind", &T::bind, "motd", &T::motd, "show-max-players",
                        &T::show_max_players, "online-mode", &T::online_mode, "servers", &T::servers);
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
    return config;
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

}  // namespace endgate
