#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/network/rtc/sctp/receiver.h"

namespace endgate::rtc::sctp {
namespace {

using namespace std::chrono_literals;

constexpr std::uint8_t beginning = DataChunk::beginning_flag;
constexpr std::uint8_t end = DataChunk::end_flag;
constexpr std::uint8_t whole = DataChunk::beginning_flag | DataChunk::end_flag;
constexpr std::uint8_t unordered = DataChunk::unordered_flag;

const Receiver::Config config{.window = 1 << 20, .max_message_size = 262144, .streams = 1024};
const auto start = std::chrono::steady_clock::time_point{} + 1h;

std::string text(const ByteBuf &buf)
{
    std::string result(buf.length(), '\0');
    buf.copyTo(std::as_writable_bytes(std::span{result}));
    return result;
}

class ReceiverTest : public testing::Test {
protected:
    Receiver::Outcome data(std::uint32_t tsn, std::uint16_t stream, std::uint16_t ssn, std::uint8_t flags,
                           const std::string &payload)
    {
        const DataChunk chunk{.tsn = tsn, .stream = stream, .ssn = ssn, .ppid = 51, .flags = flags, .payload = {}};
        return receiver.receive(chunk, ByteBuf::copyBuffer(std::as_bytes(std::span{payload})), delivered);
    }

    std::vector<std::string> take()
    {
        std::vector<std::string> result;
        for (const auto &message : delivered) {
            result.push_back(text(message.payload));
        }
        delivered.clear();
        return result;
    }

    Receiver receiver{config, 100};
    std::vector<Message> delivered;
};

TEST_F(ReceiverTest, DeliversInOrderMessagesAndAdvancesCumulativeTsn)
{
    EXPECT_EQ(data(100, 1, 0, whole, "a"), Receiver::Outcome::Accepted);
    EXPECT_EQ(data(101, 1, 1, whole, "b"), Receiver::Outcome::Accepted);
    EXPECT_EQ(take(), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(receiver.cumulativeTsn(), 101U);
    EXPECT_EQ(receiver.bufferedBytes(), 0U);

    const auto sack = receiver.makeSack(1200);
    EXPECT_EQ(sack.cumulative_tsn_ack, 101U);
    EXPECT_EQ(sack.a_rwnd, config.window);
    EXPECT_TRUE(sack.gap_blocks.empty());
    EXPECT_TRUE(sack.duplicate_tsns.empty());
}

TEST_F(ReceiverTest, AcknowledgesFirstDataImmediately)
{
    data(100, 1, 0, whole, "a");
    receiver.endPacket(start, 200ms);
    EXPECT_TRUE(receiver.sackRequired());
}

TEST_F(ReceiverTest, DelaysAcknowledgementUntilSecondPacket)
{
    data(100, 1, 0, whole, "a");
    receiver.endPacket(start, 200ms);
    receiver.makeSack(1200);

    data(101, 1, 1, whole, "b");
    receiver.endPacket(start, 200ms);
    EXPECT_FALSE(receiver.sackRequired());
    EXPECT_TRUE(receiver.sackPending());
    EXPECT_EQ(receiver.nextTimeout(), start + 200ms);

    data(102, 1, 2, whole, "c");
    receiver.endPacket(start + 10ms, 200ms);
    EXPECT_TRUE(receiver.sackRequired());
}

TEST_F(ReceiverTest, DelayedAcknowledgementFiresOnTimeout)
{
    data(100, 1, 0, whole, "a");
    receiver.endPacket(start, 200ms);
    receiver.makeSack(1200);
    data(101, 1, 1, whole, "b");
    receiver.endPacket(start, 150ms);

    receiver.handleTimeout(start + 149ms);
    EXPECT_FALSE(receiver.sackRequired());
    receiver.handleTimeout(start + 150ms);
    EXPECT_TRUE(receiver.sackRequired());
    EXPECT_FALSE(receiver.nextTimeout());
}

TEST_F(ReceiverTest, HonoursImmediateBit)
{
    data(100, 1, 0, whole, "a");
    receiver.endPacket(start, 200ms);
    receiver.makeSack(1200);
    data(101, 1, 1, whole | DataChunk::immediate_flag, "b");
    receiver.endPacket(start, 200ms);
    EXPECT_TRUE(receiver.sackRequired());
}

TEST_F(ReceiverTest, ReportsGapsImmediately)
{
    data(100, 1, 0, whole | unordered, "a");
    receiver.endPacket(start, 200ms);
    receiver.makeSack(1200);

    data(102, 1, 0, whole | unordered, "c");
    data(103, 1, 0, whole | unordered, "d");
    data(106, 1, 0, whole | unordered, "g");
    receiver.endPacket(start, 200ms);
    EXPECT_TRUE(receiver.sackRequired());
    EXPECT_EQ(take(), (std::vector<std::string>{"a", "c", "d", "g"}));

    const auto sack = receiver.makeSack(1200);
    EXPECT_EQ(sack.cumulative_tsn_ack, 100U);
    EXPECT_EQ(sack.gap_blocks, (std::vector<SackChunk::GapBlock>{{.start = 2, .end = 3}, {.start = 6, .end = 6}}));

    data(101, 1, 0, whole | unordered, "b");
    EXPECT_EQ(receiver.cumulativeTsn(), 103U);
    EXPECT_EQ(receiver.makeSack(1200).gap_blocks, (std::vector<SackChunk::GapBlock>{{.start = 3, .end = 3}}));
}

TEST_F(ReceiverTest, MergesAdjacentGapBlocks)
{
    data(102, 1, 0, whole | unordered, "c");
    data(104, 1, 0, whole | unordered, "e");
    data(103, 1, 0, whole | unordered, "d");
    EXPECT_EQ(receiver.makeSack(1200).gap_blocks, (std::vector<SackChunk::GapBlock>{{.start = 3, .end = 5}}));
}

TEST_F(ReceiverTest, ReportsDuplicatesOnce)
{
    data(100, 1, 0, whole, "a");
    receiver.makeSack(1200);
    EXPECT_EQ(data(100, 1, 0, whole, "a"), Receiver::Outcome::Duplicate);
    EXPECT_EQ(data(100, 1, 0, whole, "a"), Receiver::Outcome::Duplicate);
    data(102, 1, 2, whole, "c");
    EXPECT_EQ(data(102, 1, 2, whole, "c"), Receiver::Outcome::Duplicate);
    EXPECT_TRUE(receiver.sackRequired());
    EXPECT_EQ(take(), (std::vector<std::string>{"a"}));

    EXPECT_EQ(receiver.makeSack(1200).duplicate_tsns, (std::vector<std::uint32_t>{100, 100, 102}));
    EXPECT_TRUE(receiver.makeSack(1200).duplicate_tsns.empty());
}

TEST_F(ReceiverTest, BoundsReportedDuplicates)
{
    data(100, 1, 0, whole, "a");
    for (auto i = 0; i < 100; ++i) {
        data(100, 1, 0, whole, "a");
    }
    EXPECT_EQ(receiver.makeSack(1200).duplicate_tsns.size(), 16U);
}

TEST_F(ReceiverTest, TrimsSackToTheSpaceGiven)
{
    for (std::uint32_t i = 0; i < 40; ++i) {
        data(102 + 2 * i, 1, 0, whole | unordered, "x");
    }
    data(102, 1, 0, whole | unordered, "x");
    EXPECT_EQ(receiver.makeSack(16 + 4 * 10).gap_blocks.size(), 10U);
    EXPECT_EQ(receiver.makeSack(1200).gap_blocks.size(), 40U);
}

TEST_F(ReceiverTest, ReassemblesFragmentsWithoutGaps)
{
    data(100, 1, 0, beginning, "he");
    data(101, 1, 0, 0, "ll");
    EXPECT_TRUE(delivered.empty());
    EXPECT_GT(receiver.bufferedBytes(), 0U);
    data(102, 1, 0, end, "o");
    EXPECT_EQ(take(), (std::vector<std::string>{"hello"}));
    EXPECT_EQ(receiver.bufferedBytes(), 0U);
}

TEST_F(ReceiverTest, ReassemblesFragmentsArrivingBackwards)
{
    data(103, 1, 0, end, "d");
    data(102, 1, 0, 0, "c");
    data(101, 1, 0, 0, "b");
    EXPECT_TRUE(delivered.empty());
    data(100, 1, 0, beginning, "a");
    EXPECT_EQ(take(), (std::vector<std::string>{"abcd"}));
    EXPECT_EQ(receiver.bufferedBytes(), 0U);
}

TEST_F(ReceiverTest, KeepsUnorderedFragmentsApartFromOtherStreams)
{
    data(100, 1, 0, beginning | unordered, "x1");
    data(102, 2, 0, whole | unordered, "y");
    data(101, 1, 0, end | unordered, "x2");
    EXPECT_EQ(take(), (std::vector<std::string>{"y", "x1x2"}));
}

TEST_F(ReceiverTest, DoesNotJoinFragmentsOfDifferentMessages)
{
    data(100, 1, 0, beginning, "a");
    data(101, 1, 1, end, "b");
    EXPECT_TRUE(delivered.empty());
    data(102, 1, 0, end, "c");
    EXPECT_TRUE(delivered.empty());
}

TEST_F(ReceiverTest, HoldsOrderedMessagesForEarlierSequenceNumbers)
{
    data(101, 1, 1, whole, "second");
    data(102, 1, 2, whole, "third");
    data(103, 2, 0, whole, "other");
    EXPECT_EQ(take(), (std::vector<std::string>{"other"}));
    data(100, 1, 0, whole, "first");
    EXPECT_EQ(take(), (std::vector<std::string>{"first", "second", "third"}));
    EXPECT_EQ(receiver.bufferedBytes(), 0U);
}

TEST_F(ReceiverTest, DeliversUnorderedMessagesAsTheyArrive)
{
    data(101, 1, 0, whole | unordered, "late");
    data(100, 1, 0, whole, "early");
    EXPECT_EQ(take(), (std::vector<std::string>{"late", "early"}));
}

TEST_F(ReceiverTest, DeliversLargeMessageAsChain)
{
    const std::string fragment(1000, 'z');
    data(100, 1, 0, beginning, fragment);
    for (std::uint32_t i = 1; i < 261; ++i) {
        data(100 + i, 1, 0, 0, fragment);
    }
    data(361, 1, 0, end, std::string(1144, 'z'));
    ASSERT_EQ(delivered.size(), 1U);
    EXPECT_EQ(delivered[0].payload.length(), 262144U);
    EXPECT_FALSE(receiver.failure());
}

TEST_F(ReceiverTest, FailsOnMessageOverMaxSize)
{
    const std::string fragment(1000, 'z');
    data(100, 1, 0, beginning, fragment);
    for (std::uint32_t i = 1; i < 263; ++i) {
        data(100 + i, 1, 0, 0, fragment);
    }
    data(363, 1, 0, end, fragment);
    EXPECT_TRUE(delivered.empty());
    EXPECT_EQ(receiver.failure(), CauseCode::ProtocolViolation);
}

TEST_F(ReceiverTest, ShrinksWindowWhileFragmentsWait)
{
    data(101, 1, 1, whole, std::string(1000, 'x'));
    EXPECT_TRUE(delivered.empty());
    EXPECT_EQ(receiver.advertisedWindow(), config.window - 1000 - 64);
}

TEST_F(ReceiverTest, AcceptsOnlyTheNextTsnWhenWindowIsFull)
{
    Receiver small{{.window = 4000, .max_message_size = 2000, .streams = 16}, 100};
    std::vector<Message> out;
    const std::string payload(1000, 'x');
    const auto send = [&](std::uint32_t tsn, std::uint16_t ssn) {
        const DataChunk chunk{.tsn = tsn, .stream = 1, .ssn = ssn, .ppid = 53, .flags = whole, .payload = {}};
        return small.receive(chunk, ByteBuf::copyBuffer(std::as_bytes(std::span{payload})), out);
    };
    for (std::uint32_t i = 1; i <= 4; ++i) {
        EXPECT_EQ(send(100 + i, static_cast<std::uint16_t>(i)), Receiver::Outcome::Accepted);
    }
    EXPECT_EQ(small.advertisedWindow(), 0U);
    EXPECT_EQ(send(106, 6), Receiver::Outcome::Dropped);
    EXPECT_TRUE(small.sackRequired());
    EXPECT_EQ(send(100, 0), Receiver::Outcome::Accepted);
    EXPECT_EQ(out.size(), 5U);
    EXPECT_EQ(small.bufferedBytes(), 0U);
}

TEST_F(ReceiverTest, FailsWhenPeerKeepsFillingPastTheWindow)
{
    Receiver small{{.window = 4000, .max_message_size = 1000, .streams = 16}, 100};
    std::vector<Message> out;
    const std::string payload(1000, 'x');
    for (std::uint32_t i = 0; i < 10 && !small.failure(); ++i) {
        const DataChunk chunk{.tsn = 100 + i,
                              .stream = 1,
                              .ssn = static_cast<std::uint16_t>(i + 1),
                              .ppid = 53,
                              .flags = whole,
                              .payload = {}};
        small.receive(chunk, ByteBuf::copyBuffer(std::as_bytes(std::span{payload})), out);
    }
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(small.failure(), CauseCode::OutOfResource);
}

TEST_F(ReceiverTest, DropsTsnsFarAhead)
{
    EXPECT_EQ(data(99 + 65536, 1, 0, whole, "far"), Receiver::Outcome::Dropped);
    EXPECT_EQ(data(99 + 65535, 1, 0, whole | unordered, "edge"), Receiver::Outcome::Accepted);
    EXPECT_EQ(receiver.makeSack(1200).gap_blocks, (std::vector<SackChunk::GapBlock>{{.start = 65535, .end = 65535}}));
}

TEST_F(ReceiverTest, BoundsTrackedGaps)
{
    for (std::uint32_t i = 0; i < 256; ++i) {
        EXPECT_EQ(data(102 + 2 * i, 1, 0, whole | unordered, "x"), Receiver::Outcome::Accepted);
    }
    EXPECT_EQ(data(102 + 2 * 256, 1, 0, whole | unordered, "x"), Receiver::Outcome::Dropped);
    EXPECT_EQ(data(103, 1, 0, whole | unordered, "x"), Receiver::Outcome::Accepted);
    EXPECT_EQ(receiver.makeSack(1200).gap_blocks.size(), 255U);
}

TEST_F(ReceiverTest, FollowsTsnAcrossWrap)
{
    Receiver wrapping{config, 0xFFFFFFFE};
    std::vector<Message> out;
    for (std::uint32_t i = 0; i < 4; ++i) {
        const DataChunk chunk{.tsn = 0xFFFFFFFE + i,
                              .stream = 1,
                              .ssn = static_cast<std::uint16_t>(i),
                              .ppid = 51,
                              .flags = whole,
                              .payload = {}};
        EXPECT_EQ(wrapping.receive(chunk, ByteBuf::copyBuffer(std::vector<std::byte>(1)), out),
                  Receiver::Outcome::Accepted);
    }
    EXPECT_EQ(out.size(), 4U);
    EXPECT_EQ(wrapping.cumulativeTsn(), 1U);
}

TEST_F(ReceiverTest, FollowsSsnAcrossWrap)
{
    for (std::uint32_t i = 0; i < 65540; ++i) {
        data(100 + i, 1, static_cast<std::uint16_t>(i), whole, "m");
        if (i % 1000 == 0) {
            receiver.makeSack(1200);
        }
    }
    EXPECT_EQ(delivered.size(), 65540U);
    data(100 + 65541, 1, 5, whole, "after");
    EXPECT_EQ(delivered.size(), 65540U);
    data(100 + 65540, 1, 4, whole, "before");
    EXPECT_EQ(delivered.size(), 65542U);
    EXPECT_EQ(text(delivered.back().payload), "after");
}

TEST_F(ReceiverTest, ForwardTsnDropsAbandonedUnorderedFragments)
{
    data(101, 1, 0, beginning | unordered, "x");
    data(104, 1, 0, whole | unordered, "kept");
    EXPECT_TRUE(receiver.forwardTsn({.new_cumulative_tsn = 102, .skipped = {}}, delivered));
    EXPECT_EQ(receiver.cumulativeTsn(), 102U);
    EXPECT_EQ(take(), (std::vector<std::string>{"kept"}));
    EXPECT_EQ(receiver.bufferedBytes(), 0U);
    data(103, 1, 0, whole | unordered, "next");
    EXPECT_EQ(receiver.cumulativeTsn(), 104U);
    EXPECT_EQ(take(), (std::vector<std::string>{"next"}));
}

TEST_F(ReceiverTest, ForwardTsnReleasesStrandedOrderedMessages)
{
    data(102, 1, 2, whole, "c");
    data(103, 1, 3, whole, "d");
    EXPECT_TRUE(delivered.empty());
    EXPECT_TRUE(receiver.forwardTsn({.new_cumulative_tsn = 101, .skipped = {{.stream = 1, .ssn = 1}}}, delivered));
    EXPECT_EQ(take(), (std::vector<std::string>{"c", "d"}));
    EXPECT_EQ(receiver.cumulativeTsn(), 103U);
    EXPECT_EQ(receiver.bufferedBytes(), 0U);
}

TEST_F(ReceiverTest, ForwardTsnDeliversHeldMessagesUpToSkippedSequenceNumber)
{
    data(101, 1, 1, whole, "b");
    data(104, 1, 4, whole, "e");
    EXPECT_TRUE(receiver.forwardTsn({.new_cumulative_tsn = 102, .skipped = {{.stream = 1, .ssn = 2}}}, delivered));
    EXPECT_EQ(take(), (std::vector<std::string>{"b"}));
    data(103, 1, 3, whole, "d");
    EXPECT_EQ(take(), (std::vector<std::string>{"d", "e"}));
}

TEST_F(ReceiverTest, ForwardTsnDropsOrphanedContinuations)
{
    data(103, 1, 0, end | unordered, "tail");
    data(104, 1, 0, whole | unordered, "next");
    receiver.forwardTsn({.new_cumulative_tsn = 102, .skipped = {}}, delivered);
    EXPECT_EQ(take(), (std::vector<std::string>{"next"}));
    EXPECT_EQ(receiver.bufferedBytes(), 0U);
}

TEST_F(ReceiverTest, IgnoresStaleForwardTsnButAsksForSack)
{
    data(100, 1, 0, whole, "a");
    receiver.makeSack(1200);
    EXPECT_FALSE(receiver.forwardTsn({.new_cumulative_tsn = 100, .skipped = {}}, delivered));
    EXPECT_TRUE(receiver.sackRequired());
}

TEST_F(ReceiverTest, ResetsStreamSequenceNumbers)
{
    data(100, 1, 0, whole, "a");
    data(101, 1, 1, whole, "b");
    EXPECT_TRUE(receiver.reached(101));
    const std::vector<std::uint16_t> streams{1};
    receiver.resetStreams(streams);
    data(102, 1, 0, whole, "after reset");
    EXPECT_EQ(take(), (std::vector<std::string>{"a", "b", "after reset"}));
}

TEST_F(ReceiverTest, DefersResetUntilLastAssignedTsnArrives)
{
    data(100, 1, 0, whole, "a");
    EXPECT_FALSE(receiver.reached(102));
    receiver.deferReset(102, {1});
    data(103, 1, 0, whole, "new 0");
    data(104, 2, 0, whole, "other stream");
    data(102, 1, 2, whole, "c");
    EXPECT_EQ(take(), (std::vector<std::string>{"a", "other stream"}));
    EXPECT_FALSE(receiver.completeDeferredReset(delivered));

    data(101, 1, 1, whole, "b");
    EXPECT_EQ(take(), (std::vector<std::string>{"b", "c"}));
    const auto reset = receiver.completeDeferredReset(delivered);
    ASSERT_TRUE(reset);
    EXPECT_EQ(*reset, std::vector<std::uint16_t>{1});
    EXPECT_FALSE(receiver.resetDeferred());
    EXPECT_EQ(take(), (std::vector<std::string>{"new 0"}));
    EXPECT_EQ(receiver.bufferedBytes(), 0U);
}

TEST_F(ReceiverTest, ReplaysForwardTsnHeldBehindDeferredReset)
{
    receiver.deferReset(101, {1});
    data(100, 1, 0, whole, "old 0");
    data(103, 1, 1, whole, "new 1");
    EXPECT_TRUE(receiver.forwardTsn({.new_cumulative_tsn = 102, .skipped = {{.stream = 1, .ssn = 0}}}, delivered));
    EXPECT_EQ(take(), (std::vector<std::string>{"old 0"}));
    EXPECT_EQ(receiver.cumulativeTsn(), 103U);
    const auto reset = receiver.completeDeferredReset(delivered);
    ASSERT_TRUE(reset);
    EXPECT_EQ(take(), (std::vector<std::string>{"new 1"}));
    EXPECT_EQ(receiver.bufferedBytes(), 0U);
}

TEST_F(ReceiverTest, AcknowledgesDataItDiscards)
{
    const DataChunk chunk{.tsn = 100, .stream = 2000, .ssn = 0, .ppid = 51, .flags = whole, .payload = {}};
    EXPECT_EQ(receiver.acknowledge(chunk), Receiver::Outcome::Accepted);
    EXPECT_EQ(receiver.acknowledge(chunk), Receiver::Outcome::Duplicate);
    EXPECT_EQ(receiver.cumulativeTsn(), 100U);
    EXPECT_EQ(receiver.bufferedBytes(), 0U);
    EXPECT_TRUE(delivered.empty());
}

}  // namespace
}  // namespace endgate::rtc::sctp
