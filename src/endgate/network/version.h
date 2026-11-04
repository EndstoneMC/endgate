#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
    const auto *const end = SUPPORTED_VERSIONS.data() + SUPPORTED_VERSIONS.size();
    const auto *const found =
        std::ranges::find(SUPPORTED_VERSIONS.data(), end, protocol, &ProtocolVersion::getProtocol);
    if (found == end) {
        return std::nullopt;
    }
    return *found;
}

/**
 * The version a protocol belongs to, or the protocol's number when endgate doesn't support it.
 */
[[nodiscard]] inline std::string describeProtocol(int protocol)
{
    if (const auto version = getProtocolVersion(protocol)) {
        return std::string{version->getName()};
    }
    return std::format("protocol {}", protocol);
}

template <typename Visitor>
bool visitProtocolVersion(int protocol, Visitor &&visitor)
{
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return ((SUPPORTED_VERSIONS.at(I).getProtocol() == protocol &&
                 visitor.template operator()<SUPPORTED_VERSIONS.at(I).getProtocol()>()) ||
                ...);
    }(std::make_index_sequence<SUPPORTED_VERSIONS.size()>{});
}

}  // namespace endgate
