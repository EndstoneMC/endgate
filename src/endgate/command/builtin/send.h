#pragma once

#include "endgate/command/brigadier.h"

namespace endgate {

class ProxyServer;

/**
 * Implements the Velocity default /send command.
 *
 * @see com.velocitypowered.proxy.command.builtin.SendCommand
 */
class SendCommand {
public:
    explicit SendCommand(ProxyServer &server) : server_(server) {}

    /**
     * Registers this command.
     *
     * @see com.velocitypowered.proxy.command.builtin.SendCommand#register
     */
    void registerCommand() const;

private:
    ProxyServer &server_;
};

}  // namespace endgate
