#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <asio/ip/udp.hpp>

namespace endgate::rtc {

/**
 * @see RFC 8489 section 5
 */
enum class StunClass : std::uint16_t {
    Request = 0x0000,
    Indication = 0x0010,
    SuccessResponse = 0x0100,
    ErrorResponse = 0x0110,
};

/**
 * @see RFC 8489 section 18.2
 */
enum class StunMethod : std::uint16_t {
    Binding = 0x001,
};

/**
 * @see RFC 8489 section 18.3
 * @see RFC 8445 section 16.1
 */
enum class StunAttribute : std::uint16_t {
    Username = 0x0006,
    MessageIntegrity = 0x0008,
    ErrorCode = 0x0009,
    UnknownAttributes = 0x000A,
    MessageIntegritySha256 = 0x001C,
    XorMappedAddress = 0x0020,
    Priority = 0x0024,
    UseCandidate = 0x0025,
    Software = 0x8022,
    Fingerprint = 0x8028,
    IceControlled = 0x8029,
    IceControlling = 0x802A,
};

/**
 * @see RFC 8489 section 14.8
 */
struct StunErrorCode {
    std::uint16_t code;
    std::string reason;
};

/**
 * One STUN message, decoded from or encoded to a single datagram.
 *
 * @see RFC 8489 section 5
 */
struct StunMessage {
    using TransactionId = std::array<std::byte, 12>;

    StunClass message_class{StunClass::Request};
    StunMethod method{StunMethod::Binding};
    TransactionId transaction_id{};
    std::optional<std::string> username;
    std::optional<asio::ip::udp::endpoint> xor_mapped_address;
    std::optional<StunErrorCode> error_code;
    std::vector<std::uint16_t> unknown_attributes;
    std::optional<std::uint32_t> priority;
    bool use_candidate{false};
    std::optional<std::uint64_t> ice_controlled;
    std::optional<std::uint64_t> ice_controlling;
    std::optional<std::string> software;

    std::optional<std::size_t> integrity_offset;
    bool fingerprint{false};
    std::vector<std::uint16_t> unknown_comprehension_required;

    /**
     * @see RFC 8489 section 6.3
     */
    [[nodiscard]] static std::optional<StunMessage> decode(std::span<const std::byte> datagram);

    /**
     * @see RFC 8445 section 7.2.2
     */
    [[nodiscard]] static std::optional<std::string_view> peekLocalUfrag(std::span<const std::byte> datagram);

    /**
     * @see RFC 8489 section 6.2
     */
    [[nodiscard]] std::vector<std::byte> encode(std::optional<std::string_view> integrity_key) const;

    /**
     * @see RFC 8489 section 14.5
     */
    [[nodiscard]] bool verifyIntegrity(std::span<const std::byte> datagram, std::string_view key) const;
};

}  // namespace endgate::rtc
