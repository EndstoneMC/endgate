#pragma once

#include <memory>
#include <optional>

#include "endgate/auth/token.h"
#include "endgate/connection/connection.h"

namespace endgate {

class ProxyServer;

/**
 * Takes a client's first packet, RequestNetworkSettings. A protocol the proxy doesn't support is
 * refused; any other is answered with the proxy's own network settings, compression switches on, and
 * the connection moves on to login.
 *
 * @see com.velocitypowered.proxy.connection.client.HandshakeSessionHandler
 */
class HandshakeSessionHandler final : public MinecraftSessionHandler {
public:
    HandshakeSessionHandler(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection,
                            std::optional<GameServerToken> offer_identity);

    using MinecraftSessionHandler::handle;

    void connected() override;
    bool handle(const bp::RequestNetworkSettingsPacket &packet) override;
    void handleUnknown(ByteBufView packet) override;
    void disconnected() override {}

private:
    ProxyServer &server_;
    std::shared_ptr<MinecraftConnection> connection_;
    std::optional<GameServerToken> offer_identity_;
};

}  // namespace endgate
