#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/rtc/sctp/packet.h"

namespace endgate::rtc::sctp {

/**
 * @see dcsctp::DcSctpMessage
 */
struct Message {
    std::uint16_t stream;
    std::uint32_t ppid;
    bool unordered;
    ByteBuf payload;
};

/**
 * The data receiver of one association. It records which TSNs have arrived, decides when a SACK is due, reassembles
 * fragments without copying them and releases each stream's messages in order.
 *
 * Everything it holds is charged against the window it advertises: fragments still missing a piece, ordered messages
 * waiting on an earlier one, and data held back by a deferred stream reset.
 *
 * @see RFC 9260 section 6.2
 * @see dcsctp::DataTracker
 * @see dcsctp::ReassemblyQueue
 */
class Receiver {
public:
    using Clock = std::chrono::steady_clock;

    struct Config {
        std::size_t window;
        std::size_t max_message_size;
        std::uint16_t streams;
    };

    enum class Outcome {
        Accepted,
        Duplicate,
        Dropped,
    };

    Receiver(const Config &config, std::uint32_t peer_initial_tsn);

    Outcome receive(const DataChunk &chunk, std::vector<Message> &delivered);

    /**
     * @see RFC 9260 section 6.5
     */
    Outcome acknowledge(const DataChunk &chunk);

    /**
     * @see RFC 3758 section 3.6
     */
    bool forwardTsn(const ForwardTsnChunk &chunk, std::vector<Message> &delivered);

    void endPacket(Clock::time_point now, Clock::duration delay);
    void handleTimeout(Clock::time_point now);
    [[nodiscard]] std::optional<Clock::time_point> nextTimeout() const { return deadline_; }

    [[nodiscard]] bool sackRequired() const { return immediate_; }
    [[nodiscard]] bool sackPending() const { return immediate_ || deadline_.has_value(); }
    void requireSack() { immediate_ = true; }
    SackChunk makeSack(std::size_t max_size);

    /**
     * @see RFC 6525 section 5.2.2
     */
    [[nodiscard]] bool reached(std::uint32_t last_assigned_tsn) const;
    void resetStreams(std::span<const std::uint16_t> streams);
    void deferReset(std::uint32_t last_assigned_tsn, std::vector<std::uint16_t> streams);
    [[nodiscard]] bool resetDeferred() const { return deferred_.has_value(); }
    std::optional<std::vector<std::uint16_t>> completeDeferredReset(std::vector<Message> &delivered);

    [[nodiscard]] std::uint32_t cumulativeTsn() const { return static_cast<std::uint32_t>(cumulative_); }
    [[nodiscard]] std::size_t bufferedBytes() const { return buffered_; }
    [[nodiscard]] std::uint32_t advertisedWindow() const;
    [[nodiscard]] std::optional<CauseCode> failure() const { return failure_; }

private:
    struct Fragment {
        std::uint16_t stream;
        std::uint16_t ssn;
        std::uint32_t ppid;
        std::uint8_t flags;
        ByteBuf payload;
    };

    struct OrderedStream {
        std::uint64_t next_ssn;
        std::map<std::uint64_t, Message> blocked;
    };

    struct DeferredReset {
        std::uint64_t last_assigned_tsn;
        std::vector<std::uint16_t> streams;
    };

    using Held = std::variant<std::pair<std::uint64_t, Fragment>, ForwardTsnChunk>;

    Outcome track(const DataChunk &chunk, std::uint64_t tsn);
    void advanceCumulative();
    void assemble(std::uint64_t tsn, Fragment fragment, std::vector<Message> &delivered);
    void complete(std::uint16_t ssn, Message message, std::vector<Message> &delivered);
    void release(OrderedStream &stream, std::vector<Message> &delivered);
    void skip(std::uint64_t new_cumulative, std::span<const ForwardTsnChunk::SkippedStream> skipped,
              std::vector<Message> &delivered);
    [[nodiscard]] bool affectedByReset(std::uint64_t tsn, std::uint16_t stream) const;

    Config config_;
    std::uint64_t cumulative_;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> received_;
    std::vector<std::uint32_t> duplicates_;
    std::map<std::uint64_t, Fragment> fragments_;
    std::map<std::uint16_t, OrderedStream> ordered_;
    std::optional<DeferredReset> deferred_;
    std::vector<Held> held_;
    std::size_t buffered_{0};
    std::optional<CauseCode> failure_;

    bool data_in_packet_{false};
    bool seen_data_{false};
    bool immediate_{false};
    unsigned unacknowledged_packets_{0};
    std::optional<Clock::time_point> deadline_;
};

}  // namespace endgate::rtc::sctp
