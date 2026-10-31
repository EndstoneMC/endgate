#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "endgate/auth/token.h"
#include "endgate/connection/connection.h"
#include "endgate/connection/player.h"
#include "endgate/crypto/jose.h"

namespace endgate {

class ProxyServer;

/**
 * The login a client's LoginPacket carries: how it authenticates, the token that names it, and the
 * client data it signs with that token's key.
 *
 * @see ConnectionRequest
 * @see BaseConnectionRequest::_parseConnectionString
 */
struct ConnectionRequest {
    static constexpr int SELF_SIGNED = 2;
    static constexpr std::size_t MAX_CLIENT_DATA_SIZE = 10 * 1024 * 1024;

    int authentication_type{-1};
    JsonWebToken token;
    JsonWebToken client_data;

    [[nodiscard]] static std::optional<ConnectionRequest> fromString(std::string_view request);
};

/**
 * Takes a client's login. Online mode checks its token against Minecraft Services, and a NetherNet
 * client's against the identity its offer proved, then encrypts the connection; offline mode takes the
 * token as it is. Either way the client data must be signed with the token's key.
 *
 * @see com.velocitypowered.proxy.connection.client.InitialLoginSessionHandler
 * @see ServerNetworkHandler::handle(const NetworkIdentifier &, const LoginPacket &)
 */
class InitialLoginSessionHandler final : public MinecraftSessionHandler {
public:
    InitialLoginSessionHandler(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection,
                               std::optional<GameServerToken> offer_identity);

    using MinecraftSessionHandler::handle;

    void connected() override {}
    bool handle(const bp::LoginPacket &packet) override;
    bool handle(const bp::ClientToServerHandshakePacket &packet) override;
    void handleUnknown(ByteBufView packet) override;
    void disconnected() override {}

private:
    void finishLogin(GameProfile profile);

    ProxyServer &server_;
    spdlog::logger &logger_;
    std::shared_ptr<MinecraftConnection> connection_;
    std::optional<GameServerToken> offer_identity_;
    std::optional<GameProfile> profile_;
};

/**
 * Lets an authenticated client in: tells it the login succeeded, makes it a player and connects it to
 * the first server to try.
 *
 * @see com.velocitypowered.proxy.connection.client.AuthSessionHandler
 */
class AuthSessionHandler final : public MinecraftSessionHandler {
public:
    AuthSessionHandler(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection, GameProfile profile);

    void activated() override;
    void connected() override {}
    void handleUnknown(ByteBufView packet) override;
    void disconnected() override {}

private:
    ProxyServer &server_;
    spdlog::logger &logger_;
    std::shared_ptr<MinecraftConnection> connection_;
    GameProfile profile_;
};

}  // namespace endgate
