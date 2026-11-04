#pragma once

#include "endgate/command/brigadier.h"

namespace endgate {

class ProxyServer;

/**
 * Implements the Velocity default /glist command.
 *
 * @see com.velocitypowered.proxy.command.builtin.GlistCommand
 */
class GlistCommand {
public:
    explicit GlistCommand(ProxyServer &server) : server_(server) {}

    /**
     * Registers this command.
     *
     * @see com.velocitypowered.proxy.command.builtin.GlistCommand#register
     */
    void registerCommand() const;

private:
    ProxyServer &server_;
};

}  // namespace endgate
