#include <chrono>

#include <gtest/gtest.h>

#include "endgate/network/raknet/congestion.h"

namespace endgate::raknet {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t mtu = 1400 - UDP_HEADER_SIZE;

TEST(Congestion, StartsWithOneMtuAndTheMaximumRto)
{
    CCRakNetSlidingWindow window{mtu};
    EXPECT_EQ(window.getCwnd(), static_cast<double>(mtu));
    EXPECT_TRUE(window.isInSlowStart());
    EXPECT_EQ(window.getRtoForRetransmission(), 2s);
    EXPECT_EQ(window.getTransmissionBandwidth(0, false), mtu);
    EXPECT_EQ(window.getTransmissionBandwidth(mtu + 1, false), 0U);
    EXPECT_EQ(window.getRetransmissionBandwidth(4321), 4321U);
}

TEST(Congestion, RtoFollowsTheSmoothedRoundTrip)
{
    CCRakNetSlidingWindow window{mtu};
    window.onAck(100ms, false, 0);
    EXPECT_EQ(window.getRtoForRetransmission(), 630ms);
    window.onAck(200ms, false, 1);
    EXPECT_EQ(window.getRtoForRetransmission(), std::chrono::microseconds{2 * 105000 + 4 * 100000 + 30000});
    window.onAck(10s, false, 2);
    EXPECT_EQ(window.getRtoForRetransmission(), 2s);
}

TEST(Congestion, GrowsOnlyWhileContinuouslySending)
{
    CCRakNetSlidingWindow window{mtu};
    window.onAck(1ms, false, 0);
    EXPECT_EQ(window.getCwnd(), static_cast<double>(mtu));
    window.onAck(1ms, true, 1);
    EXPECT_EQ(window.getCwnd(), 2.0 * mtu);
    window.onAck(1ms, true, 2);
    EXPECT_EQ(window.getCwnd(), 3.0 * mtu);
}

TEST(Congestion, ResendBacksOffOncePerBlock)
{
    CCRakNetSlidingWindow window{mtu};
    for (std::uint32_t i = 0; i < 8; ++i) {
        window.onAck(1ms, true, i + 1);
    }
    EXPECT_EQ(window.getCwnd(), 9.0 * mtu);
    static_cast<void>(window.getTransmissionBandwidth(0, true));
    window.onResend();
    EXPECT_EQ(window.getCwnd(), static_cast<double>(mtu));
    EXPECT_EQ(window.getSsThresh(), 4.5 * mtu);
    window.onResend();
    EXPECT_EQ(window.getSsThresh(), 4.5 * mtu);
}

TEST(Congestion, SlowStartStopsAtTheThreshold)
{
    CCRakNetSlidingWindow window{mtu};
    for (std::uint32_t i = 0; i < 8; ++i) {
        window.onAck(1ms, true, i + 1);
    }
    static_cast<void>(window.getTransmissionBandwidth(0, true));
    window.onResend();
    for (std::uint32_t i = 0; i < 4; ++i) {
        window.onAck(1ms, true, 100 + i);
    }
    const auto threshold = 4.5 * mtu;
    EXPECT_FALSE(window.isInSlowStart());
    EXPECT_DOUBLE_EQ(window.getCwnd(), threshold + static_cast<double>(mtu) * mtu / (5.0 * mtu));
}

TEST(Congestion, NakHalvesTheThresholdWhileContinuouslySending)
{
    CCRakNetSlidingWindow window{mtu};
    window.onNak();
    EXPECT_EQ(window.getSsThresh(), 0);
    static_cast<void>(window.getTransmissionBandwidth(0, true));
    window.onNak();
    EXPECT_EQ(window.getSsThresh(), mtu / 2.0);
}

TEST(Congestion, AcksWaitForTheSynOnceTheRoundTripIsKnown)
{
    const TimePoint start{};
    CCRakNetSlidingWindow window{mtu};
    EXPECT_EQ(window.onGotPacket(0, start), 0U);
    EXPECT_TRUE(window.shouldSendAcks(start));
    window.onAck(5ms, false, 0);
    EXPECT_FALSE(window.shouldSendAcks(start + 9ms));
    EXPECT_TRUE(window.shouldSendAcks(start + 10ms));
    EXPECT_EQ(window.getAckDeadline(), start + 10ms);
    window.onSendAck();
    EXPECT_FALSE(window.getAckDeadline());
    EXPECT_TRUE(window.shouldSendAcks(start + 1ms));
}

TEST(Congestion, SkippedDatagramsAreCountedAndBounded)
{
    const TimePoint now{};
    CCRakNetSlidingWindow window{mtu};
    EXPECT_EQ(window.onGotPacket(0, now), 0U);
    EXPECT_EQ(window.onGotPacket(4, now), 3U);
    EXPECT_EQ(window.onGotPacket(2, now), 0U);
    EXPECT_EQ(window.onGotPacket(5 + 2000, now), 1000U);
    EXPECT_FALSE(window.onGotPacket(2006 + 50001, now));
    EXPECT_EQ(window.onGotPacket(2006, now), 0U);
}

TEST(Congestion, SequenceComparisonsWrapAtTwentyFourBits)
{
    EXPECT_TRUE(CCRakNetSlidingWindow::greaterThan(1, 0));
    EXPECT_TRUE(CCRakNetSlidingWindow::greaterThan(0, 0xFFFFFF));
    EXPECT_FALSE(CCRakNetSlidingWindow::greaterThan(0xFFFFFF, 0));
    EXPECT_FALSE(CCRakNetSlidingWindow::greaterThan(5, 5));
    EXPECT_TRUE(CCRakNetSlidingWindow::lessThan(0xFFFFFF, 0));
    EXPECT_TRUE(CCRakNetSlidingWindow::lessThan(0, 1));
    EXPECT_FALSE(CCRakNetSlidingWindow::lessThan(1, 0));
}

TEST(Congestion, DatagramNumbersWrap)
{
    CCRakNetSlidingWindow window{mtu};
    for (std::uint32_t i = 0; i < 0xFFFFFF; ++i) {
        static_cast<void>(window.getAndIncrementNextDatagramSequenceNumber());
    }
    EXPECT_EQ(window.getAndIncrementNextDatagramSequenceNumber(), 0xFFFFFFU);
    EXPECT_EQ(window.getNextDatagramSequenceNumber(), 0U);
}

}  // namespace
}  // namespace endgate::raknet
