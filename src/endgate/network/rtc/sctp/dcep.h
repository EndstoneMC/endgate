#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "endgate/network/buffer.h"

namespace endgate::rtc {

/**
 * @see RFC 8831 section 8
 * @see RFC 8832 section 8.1
 */
enum class Ppid : std::uint32_t {
    Dcep = 50,
    String = 51,
    BinaryPartial = 52,
    Binary = 53,
    StringPartial = 54,
    StringEmpty = 56,
    BinaryEmpty = 57,
};

/**
 * @see rtc::Reliability
 */
struct Reliability {
    bool unordered{false};
    std::optional<std::uint32_t> max_retransmits;
    std::optional<std::chrono::milliseconds> max_packet_life_time;

    bool operator==(const Reliability &) const = default;
};

/**
 * @see RFC 8832 section 5.1
 * @see rtc::impl::DataChannel
 */
struct DataChannelOpen {
    static constexpr std::uint8_t MESSAGE_TYPE = 0x03;

    std::uint8_t channel_type;
    std::uint16_t priority;
    std::uint32_t reliability_parameter;
    std::string label;
    std::string protocol;

    static std::optional<DataChannelOpen> parse(std::span<const std::byte> message);
    static DataChannelOpen of(std::string label, std::string protocol, const Reliability &reliability,
                              std::uint16_t priority);

    [[nodiscard]] std::optional<Reliability> reliability() const;
    [[nodiscard]] std::vector<std::byte> serialize() const;
};

/**
 * @see RFC 8832 section 5.2
 */
struct DataChannelAck {
    static constexpr std::uint8_t MESSAGE_TYPE = 0x02;
};

}  // namespace endgate::rtc
