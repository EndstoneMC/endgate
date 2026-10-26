#include "endgate/network/raknet/congestion.h"

#include <algorithm>
#include <cmath>

#include "endgate/network/raknet/datagram.h"

namespace endgate::raknet {

namespace {
constexpr auto SYN = std::chrono::microseconds{10000};
}  // namespace

CCRakNetSlidingWindow::CCRakNetSlidingWindow(std::size_t max_datagram_payload)
    : maximum_mtu_including_udp_header_(max_datagram_payload), cwnd_(static_cast<double>(max_datagram_payload))
{
}

std::size_t CCRakNetSlidingWindow::getTransmissionBandwidth(std::size_t unacknowledged_bytes, bool is_continuous_send)
{
    is_continuous_send_ = is_continuous_send;
    const auto unacknowledged = static_cast<double>(unacknowledged_bytes);
    if (unacknowledged <= cwnd_) {
        return static_cast<std::size_t>(cwnd_ - unacknowledged);
    }
    return 0;
}

bool CCRakNetSlidingWindow::shouldSendAcks(TimePoint now) const
{
    if (!last_rtt_) {
        return true;
    }
    return !oldest_unsent_ack_ || now >= *oldest_unsent_ack_ + SYN;
}

std::optional<TimePoint> CCRakNetSlidingWindow::getAckDeadline() const
{
    if (!last_rtt_ || !oldest_unsent_ack_) {
        return std::nullopt;
    }
    return *oldest_unsent_ack_ + SYN;
}

std::uint32_t CCRakNetSlidingWindow::getAndIncrementNextDatagramSequenceNumber()
{
    const auto number = next_datagram_sequence_number_;
    next_datagram_sequence_number_ = (next_datagram_sequence_number_ + 1) & UINT24_MASK;
    return number;
}

std::optional<std::uint32_t> CCRakNetSlidingWindow::onGotPacket(std::uint32_t datagram_sequence_number, TimePoint now)
{
    constexpr std::uint32_t skipped_resend_limit = 1000;
    constexpr std::uint32_t skipped_sanity_limit = 50000;

    if (!oldest_unsent_ack_) {
        oldest_unsent_ack_ = now;
    }
    if (datagram_sequence_number == expected_next_sequence_number_) {
        expected_next_sequence_number_ = (datagram_sequence_number + 1) & UINT24_MASK;
        return 0;
    }
    if (greaterThan(datagram_sequence_number, expected_next_sequence_number_)) {
        const auto skipped = (datagram_sequence_number - expected_next_sequence_number_) & UINT24_MASK;
        if (skipped > skipped_sanity_limit) {
            return std::nullopt;
        }
        expected_next_sequence_number_ = (datagram_sequence_number + 1) & UINT24_MASK;
        return std::min(skipped, skipped_resend_limit);
    }
    return 0;
}

void CCRakNetSlidingWindow::onResend()
{
    const auto mtu = static_cast<double>(maximum_mtu_including_udp_header_);
    if (is_continuous_send_ && !backoff_this_block_ && cwnd_ > mtu * 2) {
        ss_thresh_ = std::max(cwnd_ / 2, mtu);
        cwnd_ = mtu;
        next_congestion_control_block_ = next_datagram_sequence_number_;
        backoff_this_block_ = true;
    }
}

void CCRakNetSlidingWindow::onNak()
{
    if (is_continuous_send_ && !backoff_this_block_) {
        ss_thresh_ = cwnd_ / 2;
    }
}

void CCRakNetSlidingWindow::onAck(Duration rtt, bool is_continuous_send, std::uint32_t sequence_number)
{
    constexpr double d = 0.05;

    const auto sample = static_cast<double>(rtt.count());
    last_rtt_ = sample;
    if (!estimated_rtt_ || !deviation_rtt_) {
        estimated_rtt_ = sample;
        deviation_rtt_ = sample;
    }
    else {
        const auto difference = sample - *estimated_rtt_;
        *estimated_rtt_ += d * difference;
        *deviation_rtt_ += d * (std::abs(difference) - *deviation_rtt_);
    }

    is_continuous_send_ = is_continuous_send;
    if (!is_continuous_send) {
        return;
    }

    const bool is_new_congestion_control_period = greaterThan(sequence_number, next_congestion_control_block_);
    if (is_new_congestion_control_period) {
        backoff_this_block_ = false;
        speed_up_this_block_ = false;
        next_congestion_control_block_ = next_datagram_sequence_number_;
    }

    const auto mtu = static_cast<double>(maximum_mtu_including_udp_header_);
    if (isInSlowStart()) {
        cwnd_ += mtu;
        if (cwnd_ > ss_thresh_ && ss_thresh_ != 0) {
            cwnd_ = ss_thresh_ + mtu * mtu / cwnd_;
        }
    }
    else if (is_new_congestion_control_period) {
        cwnd_ += mtu * mtu / cwnd_;
    }
}

CCRakNetSlidingWindow::Duration CCRakNetSlidingWindow::getRtoForRetransmission() const
{
    constexpr Duration max_threshold{2000000};
    constexpr Duration additional_variance{30000};
    constexpr double u = 2.0;
    constexpr double q = 4.0;

    if (!estimated_rtt_ || !deviation_rtt_) {
        return max_threshold;
    }
    const auto threshold =
        Duration{static_cast<std::int64_t>(u * *estimated_rtt_ + q * *deviation_rtt_)} + additional_variance;
    return std::min(threshold, max_threshold);
}

bool CCRakNetSlidingWindow::greaterThan(std::uint32_t a, std::uint32_t b)
{
    constexpr std::uint32_t half_span = UINT24_MASK / 2;
    return b != a && ((b - a) & UINT24_MASK) > half_span;
}

bool CCRakNetSlidingWindow::lessThan(std::uint32_t a, std::uint32_t b)
{
    constexpr std::uint32_t half_span = UINT24_MASK / 2;
    return b != a && ((b - a) & UINT24_MASK) < half_span;
}

}  // namespace endgate::raknet
