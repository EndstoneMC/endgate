#pragma once

#include <cstdint>
#include <type_traits>

namespace endgate::rtc::sctp {

/**
 * Maps a wrapping TSN or SSN onto a 64-bit line, taking the value nearest to a reference that is already on it.
 *
 * @see RFC 9260 section 1.6
 * @see dcsctp::UnwrappedSequenceNumber
 */
template <typename T>
    requires std::is_unsigned_v<T>
constexpr std::uint64_t unwrap(T value, std::uint64_t reference)
{
    const auto distance = static_cast<std::make_signed_t<T>>(static_cast<T>(value - static_cast<T>(reference)));
    return reference + static_cast<std::int64_t>(distance);
}

}  // namespace endgate::rtc::sctp
