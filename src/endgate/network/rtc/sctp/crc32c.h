#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace endgate::rtc::sctp {

/**
 * @see RFC 9260 appendix A
 */
std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t previous = 0);

}  // namespace endgate::rtc::sctp
