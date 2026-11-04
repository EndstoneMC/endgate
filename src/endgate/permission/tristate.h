#pragma once

#include <cstdint>
#include <optional>

namespace endgate {

/**
 * Represents three different states of a setting.
 *
 * Possible values:
 * - True: a positive setting
 * - False: a negative (negated) setting
 * - Undefined: a non-existent setting
 *
 * @see com.velocitypowered.api.permission.Tristate
 */
enum class Tristate : std::uint8_t {
    True,
    False,
    Undefined,
};

/**
 * Returns a Tristate from a boolean.
 *
 * @see com.velocitypowered.api.permission.Tristate#fromBoolean
 */
[[nodiscard]] constexpr Tristate tristateFromBoolean(bool value)
{
    return value ? Tristate::True : Tristate::False;
}

/**
 * Returns a Tristate from an optional boolean.
 *
 * @see com.velocitypowered.api.permission.Tristate#fromNullableBoolean
 * @see com.velocitypowered.api.permission.Tristate#fromOptionalBoolean
 */
[[nodiscard]] constexpr Tristate tristateFromOptionalBoolean(std::optional<bool> value)
{
    return value ? tristateFromBoolean(*value) : Tristate::Undefined;
}

/**
 * Returns the value of the Tristate as a boolean.
 *
 * @see com.velocitypowered.api.permission.Tristate#asBoolean
 */
[[nodiscard]] constexpr bool asBoolean(Tristate value)
{
    return value == Tristate::True;
}

}  // namespace endgate
