#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "endgate/network/raknet/protocol.h"

namespace endgate::raknet {

/**
 * @see RakNet::CCRakNetSlidingWindow
 */
class CCRakNetSlidingWindow {
public:
    using Duration = std::chrono::microseconds;

    explicit CCRakNetSlidingWindow(std::size_t max_datagram_payload);

    [[nodiscard]] static std::size_t getRetransmissionBandwidth(std::size_t unacknowledged_bytes)
    {
        return unacknowledged_bytes;
    }

    [[nodiscard]] std::size_t getTransmissionBandwidth(std::size_t unacknowledged_bytes, bool is_continuous_send);

    /**
     * @see RakNet::CCRakNetSlidingWindow::ShouldSendACKs
     */
    [[nodiscard]] bool shouldSendAcks(TimePoint now) const;
    [[nodiscard]] std::optional<TimePoint> getAckDeadline() const;

    [[nodiscard]] std::uint32_t getNextDatagramSequenceNumber() const { return next_datagram_sequence_number_; }
    std::uint32_t getAndIncrementNextDatagramSequenceNumber();

    /**
     * @see RakNet::CCRakNetSlidingWindow::OnGotPacket
     */
    [[nodiscard]] std::optional<std::uint32_t> onGotPacket(std::uint32_t datagram_sequence_number, TimePoint now);
    void onResend();
    void onNak();
    void onAck(Duration rtt, bool is_continuous_send, std::uint32_t sequence_number);

    /**
     * @see RakNet::CCRakNetSlidingWindow::OnSendAck
     */
    void onSendAck() { oldest_unsent_ack_.reset(); }

    /**
     * @see RakNet::CCRakNetSlidingWindow::GetRTOForRetransmission
     */
    [[nodiscard]] Duration getRtoForRetransmission() const;

    [[nodiscard]] std::size_t getMtu() const { return maximum_mtu_including_udp_header_; }
    [[nodiscard]] bool isInSlowStart() const { return cwnd_ <= ss_thresh_ || ss_thresh_ == 0; }
    [[nodiscard]] double getCwnd() const { return cwnd_; }
    [[nodiscard]] double getSsThresh() const { return ss_thresh_; }
    [[nodiscard]] std::optional<double> getRtt() const { return last_rtt_; }

    [[nodiscard]] static bool greaterThan(std::uint32_t a, std::uint32_t b);
    [[nodiscard]] static bool lessThan(std::uint32_t a, std::uint32_t b);

private:
    std::size_t maximum_mtu_including_udp_header_;
    double cwnd_;
    double ss_thresh_{0};
    std::optional<TimePoint> oldest_unsent_ack_;
    std::uint32_t next_datagram_sequence_number_{0};
    std::uint32_t next_congestion_control_block_{0};
    bool backoff_this_block_{false};
    bool speed_up_this_block_{false};
    std::uint32_t expected_next_sequence_number_{0};
    bool is_continuous_send_{false};
    std::optional<double> last_rtt_;
    std::optional<double> estimated_rtt_;
    std::optional<double> deviation_rtt_;
};

}  // namespace endgate::raknet
