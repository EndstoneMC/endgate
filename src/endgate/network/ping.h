#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace endgate {

class ProxyServer;

/**
 * How a world is played, as it appears in an announcement.
 *
 * @see GameType
 */
enum class GameType {
    Survival = 0,
    Creative = 1,
    Adventure = 2,
    Default = 5,
    Spectator = 6,
};

/**
 * @see GameTypeConv::gameTypeToNonLocString
 */
[[nodiscard]] constexpr std::string_view toString(GameType type)
{
    switch (type) {
    case GameType::Survival:
        return "Survival";
    case GameType::Creative:
        return "Creative";
    case GameType::Adventure:
        return "Adventure";
    case GameType::Default:
        return "Default";
    case GameType::Spectator:
        return "Spectator";
    }
    return "";
}

[[nodiscard]] constexpr std::optional<GameType> fromString(std::string_view name)
{
    for (const auto type :
         {GameType::Survival, GameType::Creative, GameType::Adventure, GameType::Default, GameType::Spectator}) {
        if (toString(type) == name) {
            return type;
        }
    }
    return std::nullopt;
}

/**
 * What a client is told about the proxy before it connects. Every listener announces the same ping;
 * only the wire format differs.
 *
 * @see com.velocitypowered.api.proxy.server.ServerPing
 */
class ServerPing {
public:
    /**
     * @see com.velocitypowered.api.proxy.server.ServerPing.Version
     */
    class Version {
    public:
        Version(int protocol, std::string name) : protocol_(protocol), name_(std::move(name)) {}

        [[nodiscard]] int getProtocol() const { return protocol_; }

        [[nodiscard]] const std::string &getName() const { return name_; }

    private:
        int protocol_;
        std::string name_;
    };

    /**
     * @see com.velocitypowered.api.proxy.server.ServerPing.Players
     */
    class Players {
    public:
        Players(int online, int max) : online_(online), max_(max) {}

        [[nodiscard]] int getOnline() const { return online_; }

        [[nodiscard]] int getMax() const { return max_; }

    private:
        int online_;
        int max_;
    };

    ServerPing(Version version, Players players, std::string description, std::string level_name, GameType game_type)
        : version_(std::move(version)), players_(players), description_(std::move(description)),
          level_name_(std::move(level_name)), game_type_(game_type)
    {
    }

    [[nodiscard]] const Version &getVersion() const { return version_; }

    [[nodiscard]] const Players &getPlayers() const { return players_; }

    [[nodiscard]] const std::string &getDescription() const { return description_; }

    [[nodiscard]] const std::string &getLevelName() const { return level_name_; }

    [[nodiscard]] GameType getGameType() const { return game_type_; }

    [[nodiscard]] std::string toMotd(std::uint64_t guid, std::uint16_t port) const;

    [[nodiscard]] static std::optional<ServerPing> parseMotd(std::string_view announcement);

private:
    Version version_;
    Players players_;
    std::string description_;
    std::string level_name_;
    GameType game_type_;
};

/**
 * Builds the ping every listener announces.
 *
 * @see com.velocitypowered.proxy.connection.util.ServerListPingHandler
 */
class ServerListPingHandler {
public:
    explicit ServerListPingHandler(ProxyServer &server) : server_(server) {}

    /**
     * Shows the client its own protocol when the proxy supports it, and the newest one otherwise.
     *
     * @see com.velocitypowered.proxy.connection.util.ServerListPingHandler#getInitialPing
     */
    [[nodiscard]] ServerPing getInitialPing(std::optional<int> protocol) const;

private:
    ProxyServer &server_;
};

}  // namespace endgate
