#pragma once

#include "endgate/command/brigadier.h"

namespace endgate {

class ProxyServer;

/**
 * Implements Velocity's /server command.
 *
 * @see com.velocitypowered.proxy.command.builtin.ServerCommand
 */
class ServerCommand {
public:
    static constexpr int MAX_SERVERS_TO_LIST = 50;

    [[nodiscard]] static BrigadierCommand create(ProxyServer &server);
};

}  // namespace endgate
