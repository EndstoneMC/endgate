#pragma once

#include "endgate/command/brigadier.h"

namespace endgate {

class ProxyServer;

/**
 * Shuts down the proxy.
 *
 * @see com.velocitypowered.proxy.command.builtin.ShutdownCommand
 */
class ShutdownCommand {
public:
    [[nodiscard]] static BrigadierCommand command(ProxyServer &server);
};

}  // namespace endgate
