#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "endgate/auth/keys.h"
#include "endgate/crypto/jose.h"

namespace endgate {

/**
 * The token Minecraft Services issues a player to join a server with, as a login or a NetherNet offer
 * carries it.
 *
 * @see GameServerToken
 */
class GameServerToken {
public:
    static constexpr std::string_view MULTIPLAYER_AUDIENCE = "api://auth-minecraft-services/multiplayer";

    /**
     * @see DisconnectFailReason
     */
    enum class Error {
        NotAuthenticated = 46,
        DiscoveryEnvironmentMismatch = 123,
        HostWithoutKeys = 124,
        ExpiredToken = 144,
    };

    GameServerToken(std::string xuid, std::string client_public_key)
        : xuid_(std::move(xuid)), client_public_key_(std::move(client_public_key))
    {
    }

    /**
     * Checks the token's expiry against the service's clock, its signature against the service's key
     * for its kid, and its audience, in that order.
     *
     * @see GameServerToken::validate
     */
    [[nodiscard]] static std::expected<GameServerToken, Error> validate(const JsonWebToken &token,
                                                                        MinecraftServiceKeyManager &keys);

    [[nodiscard]] const std::string &getXuid() const { return xuid_; }
    [[nodiscard]] const std::string &getClientPublicKey() const { return client_public_key_; }

private:
    std::string xuid_;
    std::string client_public_key_;
};

}  // namespace endgate
