#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/network/rtc/sctp/sender.h"

namespace endgate::rtc::sctp {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t mtu = 1191;
constexpr std::size_t fragment_size = 1188 - 12 - 16;
const Sender::Config config{
    .mtu = mtu,
    .max_buffered = 1 << 20,
    .rto_initial = 500ms,
    .rto_min = 400ms,
    .rto_max = 60s,
    .partial_reliability = true,
};
const auto start = std::chrono::steady_clock::time_point{} + 1h;

struct Sent {
    std::uint32_t tsn;
    std::uint16_t stream;
    std::uint16_t ssn;
    std::uint8_t flags;
    std::size_t length;
};

ByteBuf payload(std::size_t size)
{
    return ByteBuf::copyBuffer(std::vector<std::byte>(size, std::byte{0x5a}));
}

class SenderTest : public testing::Test {
protected:
    std::vector<Sent> transmit(std::chrono::steady_clock::time_point now, std::size_t max_packets = 100)
    {
        std::vector<Sent> sent;
        for (std::size_t i = 0; i < max_packets; ++i) {
            PacketBuilder builder(buffer, {.source_port = 5000, .destination_port = 5000, .verification_tag = 1}, mtu);
            const auto added =
                sender.fastRetransmitPending() ? sender.addFastRetransmit(builder, now) : sender.addData(builder, now);
            if (!added) {
                break;
            }
            const auto packet = Packet::parse(builder.build());
            EXPECT_TRUE(packet);
            for (const auto &chunk : packet->chunks) {
                const auto data = DataChunk::parse(chunk);
                sent.push_back({data->tsn, data->stream, data->ssn, data->flags, data->payload.size()});
            }
            ++packets;
        }
        return sent;
    }

    Sender::SackOutcome sack(std::uint32_t cumulative, std::vector<SackChunk::GapBlock> gaps = {},
                             std::uint32_t window = 1 << 20, std::chrono::steady_clock::time_point now = start)
    {
        return sender.handleSack(
            {.cumulative_tsn_ack = cumulative, .a_rwnd = window, .gap_blocks = std::move(gaps), .duplicate_tsns = {}},
            now);
    }

    Sender sender{config, 1000, 1 << 20};
    std::vector<std::byte> buffer;
    std::size_t packets{0};
};

TEST_F(SenderTest, AssignsTsnsAndPerStreamSequenceNumbers)
{
    ASSERT_TRUE(sender.send(1, 51, payload(10), {}, start));
    ASSERT_TRUE(sender.send(1, 51, payload(10), {}, start));
    ASSERT_TRUE(sender.send(3, 53, payload(10), {.unordered = true}, start));
    ASSERT_TRUE(sender.send(3, 53, payload(10), {}, start));
    const auto sent = transmit(start);
    ASSERT_EQ(sent.size(), 4U);
    EXPECT_EQ(packets, 1U);
    EXPECT_EQ(sent[0].tsn, 1000U);
    EXPECT_EQ(sent[0].stream, 1);
    EXPECT_EQ(sent[0].ssn, 0);
    EXPECT_EQ(sent[0].flags, DataChunk::BeginningFlag | DataChunk::EndFlag);
    EXPECT_EQ(sent[1].tsn, 1001U);
    EXPECT_EQ(sent[1].stream, 3);
    EXPECT_EQ(sent[1].flags, DataChunk::BeginningFlag | DataChunk::EndFlag | DataChunk::UnorderedFlag);
    EXPECT_EQ(sent[2].stream, 1);
    EXPECT_EQ(sent[2].ssn, 1);
    EXPECT_EQ(sent[3].stream, 3);
    EXPECT_EQ(sent[3].ssn, 0);
    EXPECT_EQ(sender.lastAssignedTsn(), 1003U);
}

TEST_F(SenderTest, FragmentsToThePacketSize)
{
    ASSERT_TRUE(sender.send(2, 53, payload(3000), {}, start));
    const auto sent = transmit(start);
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(packets, 3U);
    EXPECT_EQ(sent[0].length, fragment_size);
    EXPECT_EQ(sent[0].flags, DataChunk::BeginningFlag);
    EXPECT_EQ(sent[1].length, fragment_size);
    EXPECT_EQ(sent[1].flags, 0);
    EXPECT_EQ(sent[2].length, 3000 - 2 * fragment_size);
    EXPECT_EQ(sent[2].flags, DataChunk::EndFlag);
    for (const auto &chunk : sent) {
        EXPECT_EQ(chunk.ssn, 0);
        EXPECT_EQ(chunk.stream, 2);
    }
    EXPECT_EQ(sent[2].tsn, sent[0].tsn + 2);
}

TEST_F(SenderTest, KeepsSmallMessagesWholeInTheNextPacket)
{
    ASSERT_TRUE(sender.send(0, 53, payload(fragment_size - 100), {}, start));
    ASSERT_TRUE(sender.send(0, 53, payload(500), {}, start));
    const auto sent = transmit(start);
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(packets, 2U);
    EXPECT_EQ(sent[1].length, 500U);
    EXPECT_EQ(sent[1].flags, DataChunk::BeginningFlag | DataChunk::EndFlag);
}

TEST_F(SenderTest, StopsAtTheCongestionWindow)
{
    for (auto i = 0; i < 20; ++i) {
        ASSERT_TRUE(sender.send(0, 53, payload(fragment_size), {}, start));
    }
    const auto sent = transmit(start);
    EXPECT_EQ(sent.size(), 11U);
    EXPECT_GE(sender.flightSize(), sender.cwnd());
    EXPECT_LT(sender.flightSize(), sender.cwnd() + mtu);
}

TEST_F(SenderTest, GrowsWindowInSlowStart)
{
    for (auto i = 0; i < 20; ++i) {
        ASSERT_TRUE(sender.send(0, 53, payload(fragment_size), {}, start));
    }
    transmit(start);
    const auto before = sender.cwnd();
    EXPECT_EQ(sack(1001), Sender::SackOutcome::Progressed);
    EXPECT_EQ(sender.cwnd(), before + mtu);
    EXPECT_EQ(sender.outstandingChunks(), 9U);
}

TEST_F(SenderTest, DropsSacksOutOfOrderOrBeyondWhatWasSent)
{
    ASSERT_TRUE(sender.send(0, 53, payload(10), {}, start));
    ASSERT_TRUE(sender.send(0, 53, payload(10), {}, start));
    transmit(start);
    EXPECT_EQ(sack(1002), Sender::SackOutcome::Dropped);
    EXPECT_EQ(sack(1000), Sender::SackOutcome::Progressed);
    EXPECT_EQ(sack(999), Sender::SackOutcome::Dropped);
    EXPECT_EQ(sack(1000), Sender::SackOutcome::Stalled);
    EXPECT_EQ(sender.outstandingChunks(), 1U);
}

TEST_F(SenderTest, IgnoresMalformedGapBlocks)
{
    for (auto i = 0; i < 4; ++i) {
        ASSERT_TRUE(sender.send(0, 53, payload(10), {}, start));
    }
    transmit(start);
    EXPECT_EQ(sack(999, {{.start = 0, .end = 1}, {.start = 3, .end = 2}, {.start = 9, .end = 20}}),
              Sender::SackOutcome::Stalled);
    EXPECT_EQ(sender.outstandingChunks(), 4U);
    EXPECT_EQ(sack(999, {{.start = 3, .end = 4}, {.start = 2, .end = 9000}}), Sender::SackOutcome::Progressed);
    EXPECT_EQ(sack(1003), Sender::SackOutcome::Progressed);
    EXPECT_TRUE(sender.idle());
}

TEST_F(SenderTest, FastRetransmitsAfterThreeMissIndications)
{
    for (auto i = 0; i < 10; ++i) {
        ASSERT_TRUE(sender.send(0, 53, payload(100), {}, start));
    }
    transmit(start);
    const auto cwnd = sender.cwnd();
    sack(999, {{.start = 2, .end = 2}});
    sack(999, {{.start = 2, .end = 3}});
    EXPECT_FALSE(sender.fastRetransmitPending());
    sack(999, {{.start = 2, .end = 4}});
    EXPECT_TRUE(sender.fastRetransmitPending());
    EXPECT_EQ(sender.cwnd(), std::max(cwnd / 2, 4 * mtu));

    const auto sent = transmit(start + 10ms);
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].tsn, 1000U);

    sack(999, {{.start = 2, .end = 5}});
    sack(999, {{.start = 2, .end = 6}});
    sack(999, {{.start = 2, .end = 7}});
    EXPECT_FALSE(sender.fastRetransmitPending());
}

TEST_F(SenderTest, RetransmitsEverythingOnTimeout)
{
    for (auto i = 0; i < 3; ++i) {
        ASSERT_TRUE(sender.send(0, 53, payload(100), {}, start));
    }
    transmit(start);
    ASSERT_EQ(sender.nextTimeout(), start + 500ms);
    EXPECT_FALSE(sender.handleTimeout(start + 499ms));
    EXPECT_TRUE(sender.handleTimeout(start + 500ms));
    EXPECT_EQ(sender.cwnd(), mtu);
    EXPECT_EQ(sender.rto(), 1s);
    EXPECT_EQ(sender.nextTimeout(), start + 1500ms);

    const auto sent = transmit(start + 500ms);
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[0].tsn, 1000U);
    EXPECT_EQ(sent[2].tsn, 1002U);
    EXPECT_EQ(sack(1002, {}, 1 << 20, start + 600ms), Sender::SackOutcome::Progressed);
    EXPECT_FALSE(sender.nextTimeout());
}

TEST_F(SenderTest, RetransmitsBeforeSendingNewData)
{
    for (auto i = 0; i < 2; ++i) {
        ASSERT_TRUE(sender.send(0, 53, payload(100), {}, start));
    }
    transmit(start);
    sender.handleTimeout(start + 500ms);
    ASSERT_TRUE(sender.send(0, 53, payload(100), {}, start));
    const auto sent = transmit(start + 500ms);
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[0].tsn, 1000U);
    EXPECT_EQ(sent[1].tsn, 1001U);
    EXPECT_EQ(sent[2].tsn, 1002U);
}

TEST_F(SenderTest, AbandonsUnreliableMessageInsteadOfRetransmitting)
{
    const Sender::SendOptions unreliable{.unordered = true, .max_retransmits = 0};
    ASSERT_TRUE(sender.send(1, 53, payload(100), unreliable, start));
    ASSERT_TRUE(sender.send(1, 53, payload(100), unreliable, start));
    transmit(start);
    EXPECT_FALSE(sender.forwardTsn(start, 1188));
    EXPECT_TRUE(sender.handleTimeout(start + 500ms));
    EXPECT_TRUE(transmit(start + 500ms).empty());
    EXPECT_EQ(sender.flightSize(), 0U);

    const auto forward = sender.forwardTsn(start + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1001U);
    EXPECT_TRUE(forward->skipped.empty());

    EXPECT_EQ(sack(1001), Sender::SackOutcome::Progressed);
    EXPECT_TRUE(sender.idle());
    EXPECT_FALSE(sender.forwardTsn(start + 1s, 1188));
}

TEST_F(SenderTest, AbandonsOnFastRetransmitAndReportsOrderedStreams)
{
    const Sender::SendOptions unreliable{.max_retransmits = 0};
    for (auto i = 0; i < 5; ++i) {
        ASSERT_TRUE(sender.send(4, 53, payload(100), unreliable, start));
    }
    transmit(start);
    sack(999, {{.start = 3, .end = 3}});
    sack(999, {{.start = 3, .end = 4}});
    sack(999, {{.start = 3, .end = 5}});
    EXPECT_FALSE(sender.fastRetransmitPending());
    const auto forward = sender.forwardTsn(start, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1001U);
    EXPECT_EQ(forward->skipped, (std::vector<ForwardTsnChunk::SkippedStream>{{.stream = 4, .ssn = 1}}));
}

TEST_F(SenderTest, DiscardsTheUnsentRestOfAnAbandonedMessage)
{
    Sender narrow{config, 1000, 1500};
    ASSERT_TRUE(narrow.send(1, 53, payload(5000), {.unordered = true, .max_retransmits = 0}, start));
    ASSERT_TRUE(narrow.send(1, 53, payload(10), {.unordered = true, .max_retransmits = 0}, start));
    PacketBuilder first(buffer, {}, mtu);
    ASSERT_TRUE(narrow.addData(first, start));
    PacketBuilder second(buffer, {}, mtu);
    ASSERT_TRUE(narrow.addData(second, start));
    PacketBuilder third(buffer, {}, mtu);
    EXPECT_FALSE(narrow.addData(third, start));
    EXPECT_EQ(narrow.outstandingChunks(), 2U);

    EXPECT_TRUE(narrow.handleTimeout(start + 500ms));
    EXPECT_EQ(narrow.bufferedAmount(), 10U);
    const auto forward = narrow.forwardTsn(start + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1001U);

    PacketBuilder next(buffer, {}, mtu);
    ASSERT_TRUE(narrow.addData(next, start + 500ms));
    const auto packet = Packet::parse(next.build());
    ASSERT_TRUE(packet);
    const auto data = DataChunk::parse(packet->chunks[0]);
    EXPECT_EQ(data->tsn, 1002U);
    EXPECT_EQ(data->flags & DataChunk::BeginningFlag, DataChunk::BeginningFlag);
}

TEST_F(SenderTest, AbandonsEveryFragmentOfTheMessage)
{
    ASSERT_TRUE(sender.send(1, 53, payload(100), {.max_retransmits = 0}, start));
    ASSERT_TRUE(sender.send(1, 53, payload(3000), {.max_retransmits = 0}, start));
    ASSERT_TRUE(sender.send(1, 53, payload(100), {}, start));
    transmit(start);
    ASSERT_EQ(sender.outstandingChunks(), 5U);
    sack(999, {{.start = 2, .end = 2}, {.start = 5, .end = 5}});
    sender.handleTimeout(start + 500ms);
    const auto forward = sender.forwardTsn(start + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1003U);
    EXPECT_EQ(forward->skipped, (std::vector<ForwardTsnChunk::SkippedStream>{{.stream = 1, .ssn = 1}}));
    EXPECT_TRUE(transmit(start + 500ms).empty());
}

TEST_F(SenderTest, RetransmitsLimitedTimesThenAbandons)
{
    ASSERT_TRUE(sender.send(0, 53, payload(100), {.max_retransmits = 1}, start));
    transmit(start);
    sender.handleTimeout(start + 500ms);
    EXPECT_EQ(transmit(start + 500ms).size(), 1U);
    sender.handleTimeout(start + 1500ms);
    EXPECT_TRUE(transmit(start + 1500ms).empty());
    EXPECT_TRUE(sender.forwardTsn(start + 1500ms, 1188));
}

TEST_F(SenderTest, ExpiresMessagesPastTheirLifetime)
{
    ASSERT_TRUE(sender.send(0, 53, payload(100), {.lifetime = 100ms}, start));
    ASSERT_TRUE(sender.send(0, 53, payload(100), {.lifetime = 100ms}, start));
    EXPECT_EQ(transmit(start).size(), 2U);
    ASSERT_TRUE(sender.send(0, 53, payload(100), {.lifetime = 100ms}, start));
    sender.handleTimeout(start + 500ms);
    EXPECT_TRUE(transmit(start + 500ms).empty());
    EXPECT_EQ(sender.bufferedAmount(), 0U);
    const auto forward = sender.forwardTsn(start + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1001U);
}

TEST_F(SenderTest, ForwardTsnSkipsAcknowledgedUnreliableMessages)
{
    const Sender::SendOptions unreliable{.unordered = true, .max_retransmits = 0};
    for (auto i = 0; i < 7; ++i) {
        ASSERT_TRUE(sender.send(1, 53, payload(100), unreliable, start));
    }
    transmit(start);
    sack(999, {{.start = 2, .end = 2}, {.start = 4, .end = 4}, {.start = 7, .end = 7}});
    sender.handleTimeout(start + 500ms);
    const auto forward = sender.forwardTsn(start + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1005U);
}

TEST_F(SenderTest, ForwardTsnStopsAtAcknowledgedReliableMessage)
{
    const Sender::SendOptions unreliable{.unordered = true, .max_retransmits = 0};
    ASSERT_TRUE(sender.send(1, 53, payload(100), unreliable, start));
    ASSERT_TRUE(sender.send(0, 53, payload(100), {}, start));
    ASSERT_TRUE(sender.send(1, 53, payload(100), unreliable, start));
    transmit(start);
    sack(999, {{.start = 2, .end = 2}});
    sender.handleTimeout(start + 500ms);
    const auto forward = sender.forwardTsn(start + 500ms, 1188);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->new_cumulative_tsn, 1000U);
}

TEST_F(SenderTest, RateLimitsRepeatedForwardTsn)
{
    ASSERT_TRUE(sender.send(0, 53, payload(100), {.unordered = true, .max_retransmits = 0}, start));
    transmit(start);
    sender.handleTimeout(start + 500ms);
    EXPECT_TRUE(sender.forwardTsn(start + 500ms, 1188));
    EXPECT_FALSE(sender.forwardTsn(start + 600ms, 1188));
    EXPECT_TRUE(sender.forwardTsn(start + 700ms, 1188));
}

TEST_F(SenderTest, ProbesZeroWindowWithOneChunk)
{
    Sender closed{config, 1000, 0};
    for (auto i = 0; i < 3; ++i) {
        ASSERT_TRUE(closed.send(0, 53, payload(100), {}, start));
    }
    PacketBuilder probe(buffer, {}, mtu);
    ASSERT_TRUE(closed.addData(probe, start));
    EXPECT_EQ(Packet::parse(probe.build())->chunks.size(), 1U);
    PacketBuilder blocked(buffer, {}, mtu);
    EXPECT_FALSE(closed.addData(blocked, start));
    closed.handleSack({.cumulative_tsn_ack = 1000, .a_rwnd = 100000, .gap_blocks = {}, .duplicate_tsns = {}}, start);
    PacketBuilder open(buffer, {}, mtu);
    ASSERT_TRUE(closed.addData(open, start));
    EXPECT_EQ(Packet::parse(open.build())->chunks.size(), 2U);
}

TEST_F(SenderTest, RespectsPeerWindowWhileDataIsInFlight)
{
    Sender narrow{config, 1000, 250};
    for (auto i = 0; i < 4; ++i) {
        ASSERT_TRUE(narrow.send(0, 53, payload(100), {}, start));
    }
    PacketBuilder builder(buffer, {}, mtu);
    ASSERT_TRUE(narrow.addData(builder, start));
    EXPECT_EQ(Packet::parse(builder.build())->chunks.size(), 2U);
}

TEST_F(SenderTest, ComputesRetransmissionTimeout)
{
    sender.observeRtt(100ms);
    EXPECT_EQ(sender.srtt(), 100ms);
    EXPECT_EQ(sender.rto(), 400ms);
    sender.observeRtt(400ms);
    EXPECT_EQ(sender.srtt(), 137500us);
    EXPECT_EQ(sender.rto(), 137500us + 4 * 112500us);
    sender.observeRtt(-1ms);
    sender.observeRtt(61s);
    EXPECT_EQ(sender.srtt(), 137500us);

    Sender fresh{config, 1, 1 << 20};
    fresh.observeRtt(50s);
    EXPECT_EQ(fresh.rto(), 60s);
    Sender fast{config, 1, 1 << 20};
    fast.observeRtt(1ms);
    EXPECT_EQ(fast.rto(), 400ms);
}

TEST_F(SenderTest, MeasuresRttFromFirstTransmissionsOnly)
{
    ASSERT_TRUE(sender.send(0, 53, payload(100), {}, start));
    transmit(start);
    sack(1000, {}, 1 << 20, start + 50ms);
    EXPECT_EQ(sender.srtt(), 50ms);

    ASSERT_TRUE(sender.send(0, 53, payload(100), {}, start));
    transmit(start + 100ms);
    sender.handleTimeout(start + 1s);
    transmit(start + 1s);
    sack(1001, {}, 1 << 20, start + 1100ms);
    EXPECT_EQ(sender.srtt(), 50ms);
}

TEST_F(SenderTest, SharesThePathBetweenStreams)
{
    for (auto i = 0; i < 3; ++i) {
        ASSERT_TRUE(sender.send(1, 53, payload(10), {}, start));
        ASSERT_TRUE(sender.send(2, 53, payload(10), {}, start));
    }
    const auto sent = transmit(start);
    ASSERT_EQ(sent.size(), 6U);
    EXPECT_EQ(sent[0].stream, 1);
    EXPECT_EQ(sent[1].stream, 2);
    EXPECT_EQ(sent[2].stream, 1);
    EXPECT_EQ(sent[3].stream, 2);
}

TEST_F(SenderTest, RefusesWhatTheBufferCannotHold)
{
    Sender small{{.mtu = mtu,
                  .max_buffered = 1000,
                  .rto_initial = 1s,
                  .rto_min = 1s,
                  .rto_max = 1s,
                  .partial_reliability = true},
                 1,
                 1 << 20};
    EXPECT_TRUE(small.send(0, 53, payload(600), {}, start));
    EXPECT_FALSE(small.send(0, 53, payload(600), {}, start));
    EXPECT_TRUE(small.send(0, 53, payload(200), {}, start));
    EXPECT_FALSE(small.send(0, 53, payload(10), {}, start));
    EXPECT_FALSE(small.send(0, 53, ByteBuf{}, {}, start));
    EXPECT_EQ(small.bufferedAmount(), 800U);
    EXPECT_EQ(small.bufferedAmount(0), 800U);
}

TEST_F(SenderTest, BoundsWhatAPeerThatNeverAdvancesCanMakeItHold)
{
    Sender held{{.mtu = mtu,
                 .max_buffered = 20000,
                 .rto_initial = 1s,
                 .rto_min = 1s,
                 .rto_max = 1s,
                 .partial_reliability = true},
                1000,
                1 << 30};
    for (auto i = 0; i < 1000; ++i) {
        held.send(0, 53, payload(100), {}, start);
    }
    std::size_t sent = 0;
    for (auto round = 0; round < 1000; ++round) {
        PacketBuilder builder(buffer, {}, mtu);
        if (!held.addData(builder, start)) {
            if (sent == held.outstandingChunks()) {
                break;
            }
        }
        sent = held.outstandingChunks();
        std::vector<SackChunk::GapBlock> gaps{{.start = 2, .end = static_cast<std::uint16_t>(sent)}};
        held.handleSack({.cumulative_tsn_ack = 999, .a_rwnd = 1 << 30, .gap_blocks = gaps, .duplicate_tsns = {}},
                        start);
    }
    EXPECT_LE(held.outstandingChunks() * 100, 20000U + 1200);
}

TEST_F(SenderTest, ResetsStreamOnceItsQueueHasDrained)
{
    ASSERT_TRUE(sender.send(5, 53, payload(10), {}, start));
    ASSERT_TRUE(sender.send(5, 53, payload(10), {}, start));
    sender.prepareReset(5);
    EXPECT_FALSE(sender.send(5, 53, payload(10), {}, start));
    EXPECT_TRUE(sender.takeStreamsReadyToReset().empty());
    transmit(start);
    EXPECT_EQ(sender.takeStreamsReadyToReset(), std::vector<std::uint16_t>{5});
    EXPECT_EQ(sender.lastAssignedTsn(), 1001U);
    EXPECT_TRUE(sender.takeStreamsReadyToReset().empty());

    const std::vector<std::uint16_t> streams{5};
    sender.finishReset(streams, true);
    ASSERT_TRUE(sender.send(5, 53, payload(10), {}, start));
    const auto sent = transmit(start);
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].ssn, 0);
}

TEST_F(SenderTest, ReopensStreamWhenResetIsDenied)
{
    ASSERT_TRUE(sender.send(5, 53, payload(10), {}, start));
    transmit(start);
    sender.prepareReset(5);
    EXPECT_EQ(sender.takeStreamsReadyToReset(), std::vector<std::uint16_t>{5});
    const std::vector<std::uint16_t> streams{5};
    sender.finishReset(streams, false);
    ASSERT_TRUE(sender.send(5, 53, payload(10), {}, start));
    const auto sent = transmit(start);
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].ssn, 1);
}

TEST_F(SenderTest, FollowsTsnAcrossWrap)
{
    Sender wrapping{config, 0xFFFFFFFF, 1 << 20};
    ASSERT_TRUE(wrapping.send(0, 53, payload(10), {}, start));
    ASSERT_TRUE(wrapping.send(0, 53, payload(10), {}, start));
    PacketBuilder builder(buffer, {}, mtu);
    ASSERT_TRUE(wrapping.addData(builder, start));
    const auto packet = Packet::parse(builder.build());
    EXPECT_EQ(DataChunk::parse(packet->chunks[0])->tsn, 0xFFFFFFFFU);
    EXPECT_EQ(DataChunk::parse(packet->chunks[1])->tsn, 0U);
    EXPECT_EQ(wrapping.handleSack({.cumulative_tsn_ack = 0, .a_rwnd = 1 << 20, .gap_blocks = {}, .duplicate_tsns = {}},
                                  start),
              Sender::SackOutcome::Progressed);
    EXPECT_TRUE(wrapping.idle());
}

}  // namespace
}  // namespace endgate::rtc::sctp
