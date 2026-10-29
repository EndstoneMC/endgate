#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace endgate {

/**
 * A Bedrock protocol version endgate supports.
 *
 * @see com.velocitypowered.api.network.ProtocolVersion
 */
class ProtocolVersion {
public:
    constexpr ProtocolVersion(int protocol, std::string_view name) : protocol_(protocol), name_(name) {}

    [[nodiscard]] constexpr int getProtocol() const { return protocol_; }

    [[nodiscard]] constexpr std::string_view getName() const { return name_; }

private:
    int protocol_;
    std::string_view name_;
};

inline constexpr std::array SUPPORTED_VERSIONS{
    ProtocolVersion{2168, "1.26.40"},
    ProtocolVersion{2169, "1.26.45"},
    ProtocolVersion{2193, "1.26.50"},
    ProtocolVersion{2225, "1.26.60"},
};

inline constexpr ProtocolVersion MINIMUM_VERSION = SUPPORTED_VERSIONS.front();
inline constexpr ProtocolVersion MAXIMUM_VERSION = SUPPORTED_VERSIONS.back();

/**
 * @see com.velocitypowered.api.network.ProtocolVersion#getProtocolVersion
 */
[[nodiscard]] constexpr std::optional<ProtocolVersion> getProtocolVersion(int protocol)
{
    const auto *const found = std::ranges::find(SUPPORTED_VERSIONS, protocol, &ProtocolVersion::getProtocol);
    if (found == SUPPORTED_VERSIONS.end()) {
        return std::nullopt;
    }
    return *found;
}

}  // namespace endgate
