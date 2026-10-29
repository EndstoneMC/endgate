#include "endgate/auth/token.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <ranges>
#include <variant>
#include <vector>

#include <glaze/json.hpp>

namespace endgate {

namespace {

constexpr glz::opts LENIENT_READ{.error_on_unknown_keys = false, .error_on_missing_keys = true};

struct Claims {
    std::int64_t exp{};
    std::optional<std::variant<std::string, std::vector<std::string>>> aud;
    std::optional<std::string> iss;
    std::optional<std::string> xid;
    std::optional<std::string> xname;
    std::optional<std::string> cpk;

    struct glaze {
        using T = Claims;
        static constexpr auto value = glz::object("exp", &T::exp, "aud", &T::aud, "iss", &T::iss, "xid", &T::xid,
                                                  "xname", &T::xname, "cpk", &T::cpk);
    };
};

}  // namespace

std::expected<GameServerToken, DisconnectFailReason> GameServerToken::validate(const JsonWebToken &token,
                                                                               MinecraftServiceKeyManager &keys)
{
    Claims claims;
    if (glz::read<LENIENT_READ>(claims, token.claims)) {
        return std::unexpected(DisconnectFailReason::NotAuthenticated);
    }
    if (keys.now() >= std::chrono::system_clock::time_point{std::chrono::seconds{claims.exp}}) {
        return std::unexpected(DisconnectFailReason::ExpiredToken);
    }
    if (!keys.hasKeys()) {
        return std::unexpected(DisconnectFailReason::HostWithoutKeys);
    }
    const auto key = keys.getKey(token.key_id);
    if (!key || !token.verify(key.get())) {
        keys.onSignatureFailure();
        return std::unexpected(DisconnectFailReason::NotAuthenticated);
    }
    const auto audience = claims.aud.value_or(std::string{});
    const auto *const single = std::get_if<std::string>(&audience);
    const auto *const many = std::get_if<std::vector<std::string>>(&audience);
    if ((single == nullptr || *single != MULTIPLAYER_AUDIENCE) &&
        (many == nullptr || !std::ranges::contains(*many, MULTIPLAYER_AUDIENCE))) {
        return std::unexpected(claims.iss != keys.getIssuer() ? DisconnectFailReason::DiscoveryEnvironmentMismatch
                                                              : DisconnectFailReason::NotAuthenticated);
    }
    return GameServerToken(claims.xid.value_or(""), claims.xname.value_or(""), claims.cpk.value_or(""));
}

std::optional<GameServerToken> GameServerToken::parse(const JsonWebToken &token)
{
    Claims claims;
    if (glz::read<LENIENT_READ>(claims, token.claims)) {
        return std::nullopt;
    }
    return GameServerToken(claims.xid.value_or(""), claims.xname.value_or(""), claims.cpk.value_or(""));
}

}  // namespace endgate
