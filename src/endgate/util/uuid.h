#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace endgate {

/**
 * @see java.util.UUID
 */
struct UUID {
    std::array<std::uint8_t, 16> bytes{};

    /**
     * Static factory to retrieve a type 3 (name based) UUID based on the specified byte array.
     *
     * @see java.util.UUID#nameUUIDFromBytes
     */
    [[nodiscard]] static UUID nameUUIDFromBytes(std::span<const std::byte> name);

    /**
     * Static factory to retrieve a type 4 (pseudo randomly generated) UUID.
     *
     * @see java.util.UUID#randomUUID
     */
    [[nodiscard]] static UUID randomUUID();

    /**
     * Creates a UUID from its string form, or nothing when the string is not one.
     *
     * @see java.util.UUID#fromString
     */
    [[nodiscard]] static std::optional<UUID> fromString(std::string_view name);

    /**
     * @see java.util.UUID#getMostSignificantBits
     */
    [[nodiscard]] std::uint64_t getMostSignificantBits() const;

    /**
     * @see java.util.UUID#getLeastSignificantBits
     */
    [[nodiscard]] std::uint64_t getLeastSignificantBits() const;

    /**
     * @see java.util.UUID#toString
     */
    [[nodiscard]] std::string toString() const;

    auto operator<=>(const UUID &) const = default;
};

/**
 * @see com.velocitypowered.api.util.UuidUtils
 */
class UuidUtils {
public:
    /**
     * Generates a UUID for use for offline mode.
     *
     * @see com.velocitypowered.api.util.UuidUtils#generateOfflinePlayerUuid
     */
    [[nodiscard]] static UUID generateOfflinePlayerUuid(std::string_view username);

    /**
     * Generates the UUID the server gives a player with an Xbox user ID.
     *
     * @see makePlayerUUIDForXUID
     */
    [[nodiscard]] static UUID generateXuidPlayerUuid(std::string_view xuid);
};

}  // namespace endgate

template <>
struct std::hash<endgate::UUID> {
    std::size_t operator()(const endgate::UUID &uuid) const noexcept
    {
        return std::hash<std::string_view>{}(
            std::string_view{reinterpret_cast<const char *>(uuid.bytes.data()), uuid.bytes.size()});
    }
};
