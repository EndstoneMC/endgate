#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace endgate::rtc::sctp {

/**
 * Everything an INIT ACK promised, carried by the peer until it echoes it back. A MAC over the fields and the time
 * of issue keeps the responder stateless until then.
 *
 * @see RFC 9260 section 5.1.3
 * @see dcsctp::StateCookie
 */
struct StateCookie {
    using Key = std::array<std::byte, 32>;

    struct Opened;

    std::uint32_t local_tag;
    std::uint32_t peer_tag;
    std::uint32_t local_initial_tsn;
    std::uint32_t peer_initial_tsn;
    std::uint32_t peer_a_rwnd;
    std::uint16_t peer_outbound_streams;
    std::uint16_t peer_inbound_streams;
    std::uint64_t tie_tag;
    bool peer_supports_forward_tsn;
    bool peer_supports_reconfig;

    [[nodiscard]] std::vector<std::byte> seal(const Key &key, std::chrono::steady_clock::time_point now) const;

    /**
     * @see RFC 9260 section 5.1.5
     */
    static std::optional<Opened> open(const Key &key, std::span<const std::byte> data,
                                      std::chrono::steady_clock::time_point now,
                                      std::chrono::steady_clock::duration lifetime);
};

struct StateCookie::Opened {
    StateCookie cookie;
    bool expired;
};

}  // namespace endgate::rtc::sctp
