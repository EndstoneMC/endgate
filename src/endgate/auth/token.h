#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "endgate/auth/keys.h"
#include "endgate/crypto/jose.h"
#include "endgate/protocol/state.h"

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

    GameServerToken(std::string xuid, std::string display_name, std::string client_public_key)
        : xuid_(std::move(xuid)), display_name_(std::move(display_name)),
          client_public_key_(std::move(client_public_key))
    {
    }

    [[nodiscard]] static std::optional<GameServerToken> parse(const JsonWebToken &token);

    /**
     * Checks the token's expiry against the service's clock, its signature against the service's key
     * for its kid, and its audience, in that order.
     *
     * @see GameServerToken::validate
     */
    [[nodiscard]] static std::expected<GameServerToken, DisconnectFailReason> validate(
        const JsonWebToken &token, MinecraftServiceKeyManager &keys);

    [[nodiscard]] const std::string &getXuid() const { return xuid_; }
    [[nodiscard]] const std::string &getDisplayName() const { return display_name_; }
    [[nodiscard]] const std::string &getClientPublicKey() const { return client_public_key_; }

private:
    std::string xuid_;
    std::string display_name_;
    std::string client_public_key_;
};

}  // namespace endgate
