#include "endgate/network/ping.h"

#include <charconv>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "endgate/config.h"
#include "endgate/network/version.h"
#include "endgate/server.h"

namespace endgate {

ServerPing ServerListPingHandler::getInitialPing(std::optional<int> protocol) const
{
    const auto &configuration = server_.getConfiguration();
    const auto shown = protocol.and_then(getProtocolVersion).value_or(MAXIMUM_VERSION);
    return {ServerPing::Version{shown.getProtocol(), std::string{shown.getName()}},
            ServerPing::Players{0, configuration.getShowMaxPlayers()}, configuration.getMotd(),
            ProxyServer::getVersion().getName(), GameType::Survival};
}

std::string ServerPing::toMotd(std::uint64_t guid, std::uint16_t port) const
{
    constexpr int joinable_through_server_screen = 1;
    constexpr int editor_world = 0;

    return std::format("MCPE;{};{};{};{};{};{};{};{};{};{};{};{};", description_, version_.getProtocol(),
                       version_.getName(), players_.getOnline(), players_.getMax(), guid, level_name_,
                       toString(game_type_), joinable_through_server_screen, port, port, editor_world);
}

std::optional<ServerPing> ServerPing::parseMotd(std::string_view announcement)
{
    constexpr std::size_t protocol = 2;
    constexpr std::size_t level = 7;

    std::vector<std::string_view> fields;
    for (const auto field : std::views::split(announcement, ';')) {
        fields.emplace_back(field);
    }
    if (fields.size() <= protocol || fields.front() != "MCPE") {
        return std::nullopt;
    }
    const auto number = [&fields](std::size_t index) -> std::optional<int> {
        if (index >= fields.size()) {
            return std::nullopt;
        }
        const auto field = fields[index];
        int value = 0;
        const auto read = std::from_chars(field.data(), field.data() + field.size(), value);
        if (read.ec != std::errc{} || read.ptr != field.data() + field.size()) {
            return std::nullopt;
        }
        return value;
    };
    const auto version = number(protocol);
    if (!version || *version <= 0) {
        return std::nullopt;
    }
    return ServerPing{Version{*version, std::string{fields.size() > 3 ? fields[3] : ""}},
                      Players{number(4).value_or(0), number(5).value_or(0)},
                      std::string{fields.size() > 1 ? fields[1] : ""},
                      std::string{fields.size() > level ? fields[level] : ""},
                      fromString(fields.size() > 8 ? fields[8] : "").value_or(GameType::Survival)};
}

}  // namespace endgate
