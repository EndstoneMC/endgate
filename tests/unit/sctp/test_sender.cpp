#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/rtc/sctp/sender.h"

namespace endgate::rtc::sctp {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t MTU = 1191;
constexpr std::size_t FRAGMENT_SIZE = 1188 - 12 - 16;
const Sender::Config CONFIG{
    .mtu = MTU,
    .max_buffered = 1 << 20,
    .rto_initial = 500ms,
    .rto_min = 400ms,
    .rto_max = 60s,
    .partial_reliability = true,
};
const auto START = std::chrono::steady_clock::time_point{} + 1h;

struct Sent {
    std::uint32_t tsn;
    std::uint16_t stream;
    std::uint16_t ssn;
    std::uint8_t flags;
    std::size_t length;
};

ByteBuf payload(std::size_t size)
{
    return Unpooled::copiedBuffer(std::vector<std::byte>(size, std::byte{0x5a}));
}

class SenderTest : public testing::Test {
protected:
    static ByteBuf newPacket()
    {
        auto packet = Unpooled::buffer(MTU & ~std::size_t{3}, MTU & ~std::size_t{3});
        CommonHeader{.source_port = 5000, .destination_port = 5000, .verification_tag = 1}.write(packet);
        return packet;
    }

    static std::optional<Packet> parse(ByteBuf &packet)
    {
        Packet::seal(packet);
        return Packet::parse(packet.slice());
    }

    std::vector<Sent> transmit(std::chrono::steady_clock::time_point now, std::size_t max_packets = 100)
    {
        std::vector<Sent> sent;
        for (std::size_t i = 0; i < max_packets; ++i) {
            auto builder = newPacket();
            const auto added = sender_.fastRetransmitPending() ? sender_.addFastRetransmit(builder, now)
                                                               : sender_.addData(builder, now);
            if (!added) {
                break;
            }
            const auto packet = parse(builder);
            EXPECT_TRUE(packet);
            for (const auto &chunk : packet->chunks) {
                const auto data = DataChunk::parse(chunk);
                sent.push_back({.tsn = data->tsn,
                                .stream = data->stream,
                                .ssn = data->ssn,
                                .flags = data->flags,
                                .length = data->payload.readableBytes()});
            }
            ++packets_;
        }
        return sent;
    }

    Sender::SackOutcome sack(std::uint32_t cumulative, std::vector<SackChunk::GapBlock> gaps = {},
                             std::uint32_t window = 1 << 20, std::chrono::steady_clock::time_point now = START)
    {
        return sender_.handleSack(
            {.cumulative_tsn_ack = cumulative, .a_rwnd = window, .gap_blocks = std::move(gaps), .duplicate_tsns = {}},
            now);
    }

    Sender sender_{CONFIG, 1000, 1 << 20};
    std::size_t packets_{0};
};

TEST_F(SenderTest, AssignsTsnsAndPerStreamSequenceNumbers)
{
    ASSERT_TRUE(sender_.send(1, 51, payload(10), {}, START));
    ASSERT_TRUE(sender_.send(1, 51, payload(10), {}, START));
    ASSERT_TRUE(sender_.send(3, 53, payload(10), {.unordered = true}, START));
    ASSERT_TRUE(sender_.send(3, 53, payload(10), {}, START));
    const auto sent = transmit(START);
    ASSERT_EQ(sent.size(), 4U);
    EXPECT_EQ(packets_, 1U);
    EXPECT_EQ(sent[0].tsn, 1000U);
    EXPECT_EQ(sent[0].stream, 1);
    EXPECT_EQ(sent[0].ssn, 0);
    EXPECT_EQ(sent[0].flags, DataChunk::BEGINNING_FLAG | DataChunk::END_FLAG);
    EXPECT_EQ(sent[1].tsn, 1001U);
    EXPECT_EQ(sent[1].stream, 3);
    EXPECT_EQ(sent[1].flags, DataChunk::BEGINNING_FLAG | DataChunk::END_FLAG | DataChunk::UNORDERED_FLAG);
    EXPECT_EQ(sent[2].stream, 1);
    EXPECT_EQ(sent[2].ssn, 1);
    EXPECT_EQ(sent[3].stream, 3);
    EXPECT_EQ(sent[3].ssn, 0);
    EXPECT_EQ(sender_.lastAssignedTsn(), 1003U);
}

TEST_F(SenderTest, FragmentsToThePacketSize)
{
    ASSERT_TRUE(sender_.send(2, 53, payload(3000), {}, START));
    const auto sent = transmit(START);
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(packets_, 3U);
    EXPECT_EQ(sent[0].length, FRAGMENT_SIZE);
    EXPECT_EQ(sent[0].flags, DataChunk::BEGINNING_FLAG);
    EXPECT_EQ(sent[1].length, FRAGMENT_SIZE);
    EXPECT_EQ(sent[1].flags, 0);
    EXPECT_EQ(sent[2].length, 3000 - 2 * FRAGMENT_SIZE);
    EXPECT_EQ(sent[2].flags, DataChunk::END_FLAG);
    for (const auto &chunk : sent) {
        EXPECT_EQ(chunk.ssn, 0);
        EXPECT_EQ(chunk.stream, 2);
    }
    EXPECT_EQ(sent[2].tsn, sent[0].tsn + 2);
}

TEST_F(SenderTest, KeepsSmallMessagesWholeInTheNextPacket)
{
    ASSERT_TRUE(sender_.send(0, 53, payload(FRAGMENT_SIZE - 100), {}, START));
    ASSERT_TRUE(sender_.send(0, 53, payload(500), {}, START));
    const auto sent = transmit(START);
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(packets_, 2U);
    EXPECT_EQ(sent[1].length, 500U);
    EXPECT_EQ(sent[1].flags, DataChunk::BEGINNING_FLAG | DataChunk::END_FLAG);
}

TEST_F(SenderTest, StopsAtTheCongestionWindow)
{
    for (auto i = 0; i < 20; ++i) {
        ASSERT_TRUE(sender_.send(0, 53, payload(FRAGMENT_SIZE), {}, START));
    }
    const auto sent = transmit(START);
    EXPECT_EQ(sent.size(), 11U);
    EXPECT_GE(sender_.flightSize(), sender_.cwnd());
    EXPECT_LT(sender_.flightSize(), sender_.cwnd() + MTU);
}

TEST_F(SenderTest, GrowsWindowInSlowStart)
{
    for (auto i = 0; i < 20; ++i) {
        ASSERT_TRUE(sender_.send(0, 53, payload(FRAGMENT_SIZE), {}, START));
    }
    transmit(START);
    const auto before = sender_.cwnd();
    EXPECT_EQ(sack(1001), Sender::SackOutcome::Progressed);
    EXPECT_EQ(sender_.cwnd(), before + MTU);
    EXPECT_EQ(sender_.outstandingChunks(), 9U);
}

TEST_F(SenderTest, DropsSacksOutOfOrderOrBeyondWhatWasSent)
{
    ASSERT_TRUE(sender_.send(0, 53, payload(10), {}, START));
    ASSERT_TRUE(sender_.send(0, 53, payload(10), {}, START));
    transmit(START);
    EXPECT_EQ(sack(1002), Sender::SackOutcome::Dropped);
    EXPECT_EQ(sack(1000), Sender::SackOutcome::Progressed);
    EXPECT_EQ(sack(999), Sender::SackOutcome::Dropped);
    EXPECT_EQ(sack(1000), Sender::SackOutcome::Stalled);
    EXPECT_EQ(sender_.outstandingChunks(), 1U);
}

TEST_F(SenderTest, IgnoresMalformedGapBlocks)
{
    for (auto i = 0; i < 4; ++i) {
        ASSERT_TRUE(sender_.send(0, 53, payload(10), {}, START));
    }
    transmit(START);
    EXPECT_EQ(sack(999, {{.start = 0, .end = 1}, {.start = 3, .end = 2}, {.start = 9, .end = 20}}),
              Sender::SackOutcome::Stalled);
    EXPECT_EQ(sender_.outstandingChunks(), 4U);
    EXPECT_EQ(sack(999, {{.start = 3, .end = 4}, {.start = 2, .end = 9000}}), Sender::SackOutcome::Progressed);
    EXPECT_EQ(sack(1003), Sender::SackOutcome::Progressed);
    EXPECT_TRUE(sender_.idle());
}

TEST_F(SenderTest, FastRetransmitsAfterThreeMissIndications)
{
    for (auto i = 0; i < 10; ++i) {
        ASSERT_TRUE(sender_.send(0, 53, payload(100), {}, START));
    }
    transmit(START);
    const auto cwnd = sender_.cwnd();
    sack(999, {{.start = 2, .end = 2}});
    sack(999, {{.start = 2, .end = 3}});
    EXPECT_FALSE(sender_.fastRetransmitPending());
    sack(999, {{.start = 2, .end = 4}});
    EXPECT_TRUE(sender_.fastRetransmitPending());
    EXPECT_EQ(sender_.cwnd(), std::max(cwnd / 2, 4 * MTU));

    const auto sent = transmit(START + 10ms);
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].tsn, 1000U);

    sack(999, {{.start = 2, .end = 5}});
    sack(999, {{.start = 2, .end = 6}});
    sack(999, {{.start = 2, .end = 7}});
    EXPECT_FALSE(sender_.fastRetransmitPending());
}

TEST_F(SenderTest, RetransmitsEverythingOnTimeout)
{
    for (auto i = 0; i < 3; ++i) {
        ASSERT_TRUE(sender_.send(0, 53, payload(100), {}, START));
    }
    transmit(START);
    ASSERT_EQ(sender_.nextTimeout(), START + 500ms);
    EXPECT_FALSE(sender_.handleTimeout(START + 499ms));
    EXPECT_TRUE(sender_.handleTimeout(START + 500ms));
    EXPECT_EQ(sender_.cwnd(), MTU);
    EXPECT_EQ(sender_.rto(), 1s);
    EXPECT_EQ(sender_.nextTimeout(), START + 1500ms);

    const auto sent = transmit(START + 500ms);
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[0].tsn, 1000U);
    EXPECT_EQ(sent[2].tsn, 1002U);
    EXPECT_EQ(sack(1002, {}, 1 << 20, START + 600ms), Sender::SackOutcome::Progressed);
    EXPECT_FALSE(sender_.nextTimeout());
}

TEST_F(SenderTest, RetransmitsBeforeSendingNewData)
{
    for (auto i = 0; i < 2; ++i) {
        ASSERT_TRUE(sender_.send(0, 53, payload(100), {}, START));
    }
    transmit(START);
    sender_.handleTimeout(START + 500ms);
    ASSERT_TRUE(sender_.send(0, 53, payload(100), {}, START));
    const auto sent = transmit(START + 500ms);
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[0].tsn, 1000U);
    EXPECT_EQ(sent[1].tsn, 1001U);
    EXPECT_EQ(sent[2].tsn, 1002U);
}

TEST_F(SenderTest, AbandonsUnreliableMessageInsteadOfRetransmitting)
{
    const Sender::SendOptions unreliable{.unordered = true, .max_retransmits = 0};
    ASSERT_TRUE(sender_.send(1, 53, payload(100), unreliable, START));
    ASSERT_TRUE(sender_.send(1, 53, payload(100), unreliable, START));
    transmit(START);
    EXPECT_FALSE(sender_.forwardTsn(START, 1188));
    EXPECT_TRUE(sender_.handleTimeout(START + 500ms));
    EXPECT_TRUE(transmit(START + 500ms).empty());
    EXPECT_EQ(sender_.flightSize(), 0U);

    const auto forward = sender_.forwardTsn(START + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1001U);
    EXPECT_TRUE(forward->skipped.empty());

    EXPECT_EQ(sack(1001), Sender::SackOutcome::Progressed);
    EXPECT_TRUE(sender_.idle());
    EXPECT_FALSE(sender_.forwardTsn(START + 1s, 1188));
}

TEST_F(SenderTest, AbandonsOnFastRetransmitAndReportsOrderedStreams)
{
    const Sender::SendOptions unreliable{.max_retransmits = 0};
    for (auto i = 0; i < 5; ++i) {
        ASSERT_TRUE(sender_.send(4, 53, payload(100), unreliable, START));
    }
    transmit(START);
    sack(999, {{.start = 3, .end = 3}});
    sack(999, {{.start = 3, .end = 4}});
    sack(999, {{.start = 3, .end = 5}});
    EXPECT_FALSE(sender_.fastRetransmitPending());
    const auto forward = sender_.forwardTsn(START, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1001U);
    EXPECT_EQ(forward->skipped, (std::vector<ForwardTsnChunk::SkippedStream>{{.stream = 4, .ssn = 1}}));
}

TEST_F(SenderTest, DiscardsTheUnsentRestOfAnAbandonedMessage)
{
    Sender narrow{CONFIG, 1000, 1500};
    ASSERT_TRUE(narrow.send(1, 53, payload(5000), {.unordered = true, .max_retransmits = 0}, START));
    ASSERT_TRUE(narrow.send(1, 53, payload(10), {.unordered = true, .max_retransmits = 0}, START));
    auto first = newPacket();
    ASSERT_TRUE(narrow.addData(first, START));
    auto second = newPacket();
    ASSERT_TRUE(narrow.addData(second, START));
    auto third = newPacket();
    EXPECT_FALSE(narrow.addData(third, START));
    EXPECT_EQ(narrow.outstandingChunks(), 2U);

    EXPECT_TRUE(narrow.handleTimeout(START + 500ms));
    EXPECT_EQ(narrow.bufferedAmount(), 10U);
    const auto forward = narrow.forwardTsn(START + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1001U);

    auto next = newPacket();
    ASSERT_TRUE(narrow.addData(next, START + 500ms));
    const auto packet = parse(next);
    ASSERT_TRUE(packet);
    const auto data = DataChunk::parse(packet->chunks[0]);
    EXPECT_EQ(data->tsn, 1002U);
    EXPECT_EQ(data->flags & DataChunk::BEGINNING_FLAG, DataChunk::BEGINNING_FLAG);
}

TEST_F(SenderTest, AbandonsEveryFragmentOfTheMessage)
{
    ASSERT_TRUE(sender_.send(1, 53, payload(100), {.max_retransmits = 0}, START));
    ASSERT_TRUE(sender_.send(1, 53, payload(3000), {.max_retransmits = 0}, START));
    ASSERT_TRUE(sender_.send(1, 53, payload(100), {}, START));
    transmit(START);
    ASSERT_EQ(sender_.outstandingChunks(), 5U);
    sack(999, {{.start = 2, .end = 2}, {.start = 5, .end = 5}});
    sender_.handleTimeout(START + 500ms);
    const auto forward = sender_.forwardTsn(START + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1003U);
    EXPECT_EQ(forward->skipped, (std::vector<ForwardTsnChunk::SkippedStream>{{.stream = 1, .ssn = 1}}));
    EXPECT_TRUE(transmit(START + 500ms).empty());
}

TEST_F(SenderTest, RetransmitsLimitedTimesThenAbandons)
{
    ASSERT_TRUE(sender_.send(0, 53, payload(100), {.max_retransmits = 1}, START));
    transmit(START);
    sender_.handleTimeout(START + 500ms);
    EXPECT_EQ(transmit(START + 500ms).size(), 1U);
    sender_.handleTimeout(START + 1500ms);
    EXPECT_TRUE(transmit(START + 1500ms).empty());
    EXPECT_TRUE(sender_.forwardTsn(START + 1500ms, 1188));
}

TEST_F(SenderTest, ExpiresMessagesPastTheirLifetime)
{
    ASSERT_TRUE(sender_.send(0, 53, payload(100), {.lifetime = 100ms}, START));
    ASSERT_TRUE(sender_.send(0, 53, payload(100), {.lifetime = 100ms}, START));
    EXPECT_EQ(transmit(START).size(), 2U);
    ASSERT_TRUE(sender_.send(0, 53, payload(100), {.lifetime = 100ms}, START));
    sender_.handleTimeout(START + 500ms);
    EXPECT_TRUE(transmit(START + 500ms).empty());
    EXPECT_EQ(sender_.bufferedAmount(), 0U);
    const auto forward = sender_.forwardTsn(START + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1001U);
}

TEST_F(SenderTest, ForwardTsnSkipsAcknowledgedUnreliableMessages)
{
    const Sender::SendOptions unreliable{.unordered = true, .max_retransmits = 0};
    for (auto i = 0; i < 7; ++i) {
        ASSERT_TRUE(sender_.send(1, 53, payload(100), unreliable, START));
    }
    transmit(START);
    sack(999, {{.start = 2, .end = 2}, {.start = 4, .end = 4}, {.start = 7, .end = 7}});
    sender_.handleTimeout(START + 500ms);
    const auto forward = sender_.forwardTsn(START + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1005U);
}

TEST_F(SenderTest, ForwardTsnStopsAtAcknowledgedReliableMessage)
{
    const Sender::SendOptions unreliable{.unordered = true, .max_retransmits = 0};
    ASSERT_TRUE(sender_.send(1, 53, payload(100), unreliable, START));
    ASSERT_TRUE(sender_.send(0, 53, payload(100), {}, START));
    ASSERT_TRUE(sender_.send(1, 53, payload(100), unreliable, START));
    transmit(START);
    sack(999, {{.start = 2, .end = 2}});
    sender_.handleTimeout(START + 500ms);
    const auto forward = sender_.forwardTsn(START + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1000U);
}

TEST_F(SenderTest, RateLimitsRepeatedForwardTsn)
{
    ASSERT_TRUE(sender_.send(0, 53, payload(100), {.unordered = true, .max_retransmits = 0}, START));
    transmit(START);
    sender_.handleTimeout(START + 500ms);
    EXPECT_TRUE(sender_.forwardTsn(START + 500ms, 1188));
    EXPECT_FALSE(sender_.forwardTsn(START + 600ms, 1188));
    EXPECT_TRUE(sender_.forwardTsn(START + 700ms, 1188));
}

TEST_F(SenderTest, ProbesZeroWindowWithOneChunk)
{
    Sender closed{CONFIG, 1000, 0};
    for (auto i = 0; i < 3; ++i) {
        ASSERT_TRUE(closed.send(0, 53, payload(100), {}, START));
    }
    auto probe = newPacket();
    ASSERT_TRUE(closed.addData(probe, START));
    EXPECT_EQ(parse(probe)->chunks.size(), 1U);
    auto blocked = newPacket();
    EXPECT_FALSE(closed.addData(blocked, START));
    closed.handleSack({.cumulative_tsn_ack = 1000, .a_rwnd = 100000, .gap_blocks = {}, .duplicate_tsns = {}}, START);
    auto open = newPacket();
    ASSERT_TRUE(closed.addData(open, START));
    EXPECT_EQ(parse(open)->chunks.size(), 2U);
}

TEST_F(SenderTest, RespectsPeerWindowWhileDataIsInFlight)
{
    Sender narrow{CONFIG, 1000, 250};
    for (auto i = 0; i < 4; ++i) {
        ASSERT_TRUE(narrow.send(0, 53, payload(100), {}, START));
    }
    auto builder = newPacket();
    ASSERT_TRUE(narrow.addData(builder, START));
    EXPECT_EQ(parse(builder)->chunks.size(), 2U);
}

TEST_F(SenderTest, ComputesRetransmissionTimeout)
{
    sender_.observeRtt(100ms);
    EXPECT_EQ(sender_.srtt(), 100ms);
    EXPECT_EQ(sender_.rto(), 400ms);
    sender_.observeRtt(400ms);
    EXPECT_EQ(sender_.srtt(), 137500us);
    EXPECT_EQ(sender_.rto(), 137500us + 4 * 112500us);
    sender_.observeRtt(-1ms);
    sender_.observeRtt(61s);
    EXPECT_EQ(sender_.srtt(), 137500us);

    Sender fresh{CONFIG, 1, 1 << 20};
    fresh.observeRtt(50s);
    EXPECT_EQ(fresh.rto(), 60s);
    Sender fast{CONFIG, 1, 1 << 20};
    fast.observeRtt(1ms);
    EXPECT_EQ(fast.rto(), 400ms);
}

TEST_F(SenderTest, MeasuresRttFromFirstTransmissionsOnly)
{
    ASSERT_TRUE(sender_.send(0, 53, payload(100), {}, START));
    transmit(START);
    sack(1000, {}, 1 << 20, START + 50ms);
    EXPECT_EQ(sender_.srtt(), 50ms);

    ASSERT_TRUE(sender_.send(0, 53, payload(100), {}, START));
    transmit(START + 100ms);
    sender_.handleTimeout(START + 1s);
    transmit(START + 1s);
    sack(1001, {}, 1 << 20, START + 1100ms);
    EXPECT_EQ(sender_.srtt(), 50ms);
}

TEST_F(SenderTest, SharesThePathBetweenStreams)
{
    for (auto i = 0; i < 3; ++i) {
        ASSERT_TRUE(sender_.send(1, 53, payload(10), {}, START));
        ASSERT_TRUE(sender_.send(2, 53, payload(10), {}, START));
    }
    const auto sent = transmit(START);
    ASSERT_EQ(sent.size(), 6U);
    EXPECT_EQ(sent[0].stream, 1);
    EXPECT_EQ(sent[1].stream, 2);
    EXPECT_EQ(sent[2].stream, 1);
    EXPECT_EQ(sent[3].stream, 2);
}

TEST_F(SenderTest, RefusesWhatTheBufferCannotHold)
{
    Sender small{{.mtu = MTU,
                  .max_buffered = 1000,
                  .rto_initial = 1s,
                  .rto_min = 1s,
                  .rto_max = 1s,
                  .partial_reliability = true},
                 1,
                 1 << 20};
    EXPECT_TRUE(small.send(0, 53, payload(600), {}, START));
    EXPECT_FALSE(small.send(0, 53, payload(600), {}, START));
    EXPECT_TRUE(small.send(0, 53, payload(200), {}, START));
    EXPECT_FALSE(small.send(0, 53, payload(10), {}, START));
    EXPECT_FALSE(small.send(0, 53, ByteBuf{}, {}, START));
    EXPECT_EQ(small.bufferedAmount(), 800U);
    EXPECT_EQ(small.bufferedAmount(0), 800U);
}

TEST_F(SenderTest, BoundsWhatAPeerThatNeverAdvancesCanMakeItHold)
{
    Sender held{{.mtu = MTU,
                 .max_buffered = 20000,
                 .rto_initial = 1s,
                 .rto_min = 1s,
                 .rto_max = 1s,
                 .partial_reliability = true},
                1000,
                1 << 30};
    for (auto i = 0; i < 1000; ++i) {
        held.send(0, 53, payload(100), {}, START);
    }
    std::size_t sent = 0;
    for (auto round = 0; round < 1000; ++round) {
        auto builder = newPacket();
        if (!held.addData(builder, START)) {
            if (sent == held.outstandingChunks()) {
                break;
            }
        }
        sent = held.outstandingChunks();
        std::vector<SackChunk::GapBlock> gaps{{.start = 2, .end = static_cast<std::uint16_t>(sent)}};
        held.handleSack({.cumulative_tsn_ack = 999, .a_rwnd = 1 << 30, .gap_blocks = gaps, .duplicate_tsns = {}},
                        START);
    }
    EXPECT_LE(held.outstandingChunks() * 100, 20000U + 1200);
}

TEST_F(SenderTest, ResetsStreamOnceItsQueueHasDrained)
{
    ASSERT_TRUE(sender_.send(5, 53, payload(10), {}, START));
    ASSERT_TRUE(sender_.send(5, 53, payload(10), {}, START));
    sender_.prepareReset(5);
    EXPECT_FALSE(sender_.send(5, 53, payload(10), {}, START));
    EXPECT_TRUE(sender_.takeStreamsReadyToReset().empty());
    transmit(START);
    EXPECT_EQ(sender_.takeStreamsReadyToReset(), std::vector<std::uint16_t>{5});
    EXPECT_EQ(sender_.lastAssignedTsn(), 1001U);
    EXPECT_TRUE(sender_.takeStreamsReadyToReset().empty());

    const std::vector<std::uint16_t> streams{5};
    sender_.finishReset(streams, true);
    ASSERT_TRUE(sender_.send(5, 53, payload(10), {}, START));
    const auto sent = transmit(START);
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].ssn, 0);
}

TEST_F(SenderTest, ReopensStreamWhenResetIsDenied)
{
    ASSERT_TRUE(sender_.send(5, 53, payload(10), {}, START));
    transmit(START);
    sender_.prepareReset(5);
    EXPECT_EQ(sender_.takeStreamsReadyToReset(), std::vector<std::uint16_t>{5});
    const std::vector<std::uint16_t> streams{5};
    sender_.finishReset(streams, false);
    ASSERT_TRUE(sender_.send(5, 53, payload(10), {}, START));
    const auto sent = transmit(START);
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].ssn, 1);
}

TEST_F(SenderTest, FollowsTsnAcrossWrap)
{
    Sender wrapping{CONFIG, 0xFFFFFFFF, 1 << 20};
    ASSERT_TRUE(wrapping.send(0, 53, payload(10), {}, START));
    ASSERT_TRUE(wrapping.send(0, 53, payload(10), {}, START));
    auto builder = newPacket();
    ASSERT_TRUE(wrapping.addData(builder, START));
    const auto packet = parse(builder);
    EXPECT_EQ(DataChunk::parse(packet->chunks[0])->tsn, 0xFFFFFFFFU);
    EXPECT_EQ(DataChunk::parse(packet->chunks[1])->tsn, 0U);
    EXPECT_EQ(wrapping.handleSack({.cumulative_tsn_ack = 0, .a_rwnd = 1 << 20, .gap_blocks = {}, .duplicate_tsns = {}},
                                  START),
              Sender::SackOutcome::Progressed);
    EXPECT_TRUE(wrapping.idle());
}

}  // namespace
}  // namespace endgate::rtc::sctp
