#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/rtc/sctp/packet.h"

namespace endgate::rtc::sctp {

/**
 * The data sender of one association: the per-stream send queue, TSN assignment and fragmentation to the packet size,
 * the retransmission queue with its timer, congestion control, and PR-SCTP abandonment.
 *
 * @see RFC 9260 section 6.1
 * @see RFC 9260 section 6.3
 * @see RFC 9260 section 7
 * @see RFC 3758 section 3.5
 * @see dcsctp::RetransmissionQueue
 * @see dcsctp::OutstandingData
 * @see dcsctp::RRSendQueue
 */
class Sender {
public:
    using Clock = std::chrono::steady_clock;

    struct Config {
        std::size_t mtu;
        std::size_t max_buffered;
        Clock::duration rto_initial;
        Clock::duration rto_min;
        Clock::duration rto_max;
        bool partial_reliability;
    };

    struct SendOptions {
        bool unordered{false};
        std::optional<std::uint16_t> max_retransmits;
        std::optional<Clock::duration> lifetime;
    };

    enum class SackOutcome {
        Dropped,
        Stalled,
        Progressed,
    };

    Sender(const Config &config, std::uint32_t initial_tsn, std::uint32_t peer_window);

    bool send(std::uint16_t stream, std::uint32_t ppid, ByteBuf payload, const SendOptions &options,
              Clock::time_point now);

    /**
     * @see RFC 9260 section 6.2.1
     */
    SackOutcome handleSack(const SackChunk &sack, Clock::time_point now);

    /**
     * @see RFC 9260 section 6.3.3
     */
    bool handleTimeout(Clock::time_point now);
    [[nodiscard]] std::optional<Clock::time_point> nextTimeout() const { return t3_; }

    /**
     * @see RFC 9260 section 7.2.4
     */
    [[nodiscard]] bool fastRetransmitPending() const { return fast_retransmit_pending_; }
    bool addFastRetransmit(ByteBuf &packet, Clock::time_point now);
    bool addData(ByteBuf &packet, Clock::time_point now);
    [[nodiscard]] bool hasDataToSend() const { return lost_ > 0 || !ready_.empty(); }

    /**
     * @see RFC 3758 section 3.5
     */
    std::optional<ForwardTsnChunk> forwardTsn(Clock::time_point now, std::size_t max_size);

    /**
     * @see RFC 9260 section 6.3.1
     */
    void observeRtt(Clock::duration rtt);
    [[nodiscard]] Clock::duration rto() const { return rto_; }
    [[nodiscard]] Clock::duration srtt() const { return srtt_; }

    /**
     * @see RFC 6525 section 5.1.2
     */
    void prepareReset(std::uint16_t stream);
    std::vector<std::uint16_t> takeStreamsReadyToReset();
    void finishReset(std::span<const std::uint16_t> streams, bool performed);
    [[nodiscard]] std::uint32_t lastAssignedTsn() const
    {
        return static_cast<std::uint32_t>(cumulative_ack_ + outstanding_.size());
    }

    [[nodiscard]] bool idle() const { return buffered_ == 0 && outstanding_.empty(); }
    [[nodiscard]] std::size_t bufferedAmount() const { return buffered_; }
    [[nodiscard]] std::size_t bufferedAmount(std::uint16_t stream) const;
    [[nodiscard]] std::size_t cwnd() const { return cwnd_; }
    [[nodiscard]] std::size_t ssthresh() const { return ssthresh_; }
    [[nodiscard]] std::size_t flightSize() const { return flight_; }
    [[nodiscard]] std::size_t peerWindow() const { return peer_window_; }
    [[nodiscard]] std::size_t outstandingChunks() const { return outstanding_.size(); }

private:
    enum class State : std::uint8_t {
        InFlight,
        Acked,
        Lost,
        Abandoned,
    };

    struct Outstanding {
        std::uint16_t stream;
        std::uint16_t ssn;
        std::uint32_t ppid;
        std::uint8_t flags;
        ByteBuf payload;
        std::uint64_t message;
        Clock::time_point sent;
        std::uint64_t boundary;
        std::optional<std::uint16_t> max_retransmits;
        std::optional<Clock::time_point> expires_at;
        std::uint16_t transmissions{1};
        std::uint8_t misses{0};
        State state{State::InFlight};
        bool fast{false};
        bool fast_retransmitted{false};
    };

    struct Pending {
        std::uint32_t ppid;
        ByteBuf payload;
        bool unordered;
        std::optional<std::uint16_t> max_retransmits;
        std::optional<Clock::time_point> expires_at;
        std::uint64_t id;
        std::size_t offset{0};
        std::uint16_t ssn{0};
    };

    enum class Reset : std::uint8_t {
        None,
        Requested,
        InFlight,
    };

    struct OutgoingStream {
        std::deque<Pending> queue;
        std::uint16_t next_ssn{0};
        std::size_t buffered{0};
        Reset reset{Reset::None};
    };

    [[nodiscard]] std::uint64_t nextTsn() const { return cumulative_ack_ + 1 + outstanding_.size(); }
    [[nodiscard]] bool exhausted(const Outstanding &item, Clock::time_point now) const;
    void leaveFlight(Outstanding &item);
    void retransmit(std::size_t index, ByteBuf &packet, Clock::time_point now);
    void abandon(std::size_t index);
    void dropFront(std::uint16_t stream_id, OutgoingStream &stream);

    Config config_;
    std::uint64_t cumulative_ack_;
    std::deque<Outstanding> outstanding_;
    std::map<std::uint16_t, OutgoingStream> streams_;
    std::set<std::uint16_t> ready_;
    std::optional<std::uint16_t> current_;
    std::uint16_t last_stream_{0};
    std::uint64_t next_message_{0};
    std::size_t buffered_{0};
    std::size_t queued_{0};
    std::size_t held_{0};

    std::size_t flight_{0};
    std::size_t flight_payload_{0};
    std::size_t lost_{0};
    std::size_t peer_window_;
    std::size_t cwnd_;
    std::size_t ssthresh_;
    std::size_t partial_bytes_acked_{0};
    std::optional<std::uint64_t> fast_recovery_exit_;
    bool fast_retransmit_pending_{false};

    Clock::duration srtt_;
    Clock::duration rttvar_{};
    Clock::duration rto_;
    bool measured_{false};
    std::optional<Clock::time_point> t3_;

    std::uint64_t last_forward_tsn_{0};
    Clock::time_point forward_tsn_retry_at_;
};

}  // namespace endgate::rtc::sctp
