#pragma once

#include "endgate/command/brigadier.h"

namespace endgate {

class ProxyServer;

/**
 * Implements the /endgate command and friends.
 *
 * @see com.velocitypowered.proxy.command.builtin.VelocityCommand
 */
class EndgateCommand {
public:
    [[nodiscard]] static BrigadierCommand create(ProxyServer &server);
};

}  // namespace endgate
