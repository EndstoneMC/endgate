#pragma once

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "endgate/backend.h"
#include "endgate/connection/player.h"
#include "endgate/server.h"

namespace endgate {

/**
 * @see com.velocitypowered.proxy.command.builtin.BuiltinCommandUtil
 */
struct BuiltinCommandUtil {
    [[nodiscard]] static bool equalsIgnoreCase(std::string_view left, std::string_view right)
    {
        return std::ranges::equal(left, right,
                                  [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
    }

    /**
     * Whether the name starts with the argument, ignoring case.
     */
    [[nodiscard]] static bool regionMatches(std::string_view name, std::string_view argument)
    {
        return name.size() >= argument.size() && equalsIgnoreCase(name.substr(0, argument.size()), argument);
    }

    [[nodiscard]] static std::vector<BackendServer *> sortedServerList(ProxyServer &proxy)
    {
        std::vector<BackendServer *> servers = proxy.getAllServers();
        std::ranges::sort(servers, [](const BackendServer *left, const BackendServer *right) {
            return left->getServerInfo().getName() < right->getServerInfo().getName();
        });
        return servers;
    }

    /**
     * @see com.velocitypowered.api.proxy.server.RegisteredServer#getPlayersConnected
     */
    [[nodiscard]] static std::vector<std::shared_ptr<Player>> getPlayersConnected(ProxyServer &proxy,
                                                                                  const BackendServer &server)
    {
        std::vector<std::shared_ptr<Player>> players;
        for (auto &player : proxy.getAllPlayers()) {
            if (player->getCurrentServer() == &server) {
                players.push_back(std::move(player));
            }
        }
        return players;
    }
};

}  // namespace endgate
