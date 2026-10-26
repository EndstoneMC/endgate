#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/network/nethernet/framing.h"

namespace endgate::nethernet {
namespace {

constexpr std::size_t FragmentSize = PacketQueue::DEFAULT_MAX_MESSAGE_SIZE - 1;
constexpr std::size_t MaxPacketSize = 8 * 1024 * 1024;

std::vector<std::byte> bytesOf(const ByteBuf &buf)
{
    std::vector<std::byte> bytes(buf.length());
    buf.copyTo(bytes);
    return bytes;
}

std::vector<std::byte> payloadOf(std::size_t size, std::uint8_t seed)
{
    std::vector<std::byte> payload(size);
    for (std::size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<std::byte>((i * 7 + seed) & 0xFF);
    }
    return payload;
}

struct Fragment {
    ESendType type;
    std::vector<std::byte> bytes;
};

class Recorder : public PacketQueue::Listener {
public:
    void sendFragment(ESendType type, ByteBuf fragment) override { fragments.push_back({type, bytesOf(fragment)}); }
    void onPacket(ByteBuf packet) override { packets.push_back(bytesOf(packet)); }
    void closeChannel(ESendType type) override { closed.push_back(type); }

    std::vector<Fragment> fragments;
    std::vector<std::vector<std::byte>> packets;
    std::vector<ESendType> closed;
};

class PacketQueueTest : public testing::Test {
protected:
    void deliverEverySentFragment()
    {
        for (const auto &fragment : recorder.fragments) {
            const auto message = ByteBuf::copyBuffer(fragment.bytes);
            if (fragment.type == ESendType::Reliable) {
                queue.onOrderedMessage(message);
            }
            else {
                queue.onUnorderedMessage(message);
            }
        }
    }

    Recorder recorder;
    PacketQueue queue{recorder, MaxPacketSize};
};

TEST_F(PacketQueueTest, SendsASmallPacketAsOneFragmentWithHeaderZero)
{
    const std::vector payload{std::byte{'A'}};
    ASSERT_TRUE(queue.push(ByteBuf::copyBuffer(payload), ESendType::Reliable));

    ASSERT_EQ(recorder.fragments.size(), 1U);
    EXPECT_EQ(recorder.fragments[0].type, ESendType::Reliable);
    EXPECT_EQ(recorder.fragments[0].bytes, (std::vector{std::byte{0x00}, std::byte{'A'}}));
}

TEST_F(PacketQueueTest, SendsAnUnreliablePacketOnTheUnorderedChannel)
{
    const std::vector payload{std::byte{'h'}, std::byte{'i'}};
    ASSERT_TRUE(queue.push(ByteBuf::copyBuffer(payload), ESendType::Unreliable));

    ASSERT_EQ(recorder.fragments.size(), 1U);
    EXPECT_EQ(recorder.fragments[0].type, ESendType::Unreliable);
    EXPECT_EQ(recorder.fragments[0].bytes, (std::vector{std::byte{0x00}, std::byte{'h'}, std::byte{'i'}}));
}

TEST_F(PacketQueueTest, KeepsAPacketOfExactlyOneFragmentWhole)
{
    ASSERT_TRUE(queue.push(ByteBuf::copyBuffer(payloadOf(FragmentSize, 1)), ESendType::Reliable));

    ASSERT_EQ(recorder.fragments.size(), 1U);
    EXPECT_EQ(recorder.fragments[0].bytes.size(), FragmentSize + 1);
    EXPECT_EQ(recorder.fragments[0].bytes.front(), std::byte{0x00});
}

TEST_F(PacketQueueTest, CountsFragmentsDownToZero)
{
    const auto payload = payloadOf(FragmentSize * 2 + 5, 2);
    ASSERT_TRUE(queue.push(ByteBuf::copyBuffer(payload), ESendType::Reliable));

    ASSERT_EQ(recorder.fragments.size(), 3U);
    EXPECT_EQ(recorder.fragments[0].bytes.front(), std::byte{2});
    EXPECT_EQ(recorder.fragments[1].bytes.front(), std::byte{1});
    EXPECT_EQ(recorder.fragments[2].bytes.front(), std::byte{0});
    EXPECT_EQ(recorder.fragments[0].bytes.size(), FragmentSize + 1);
    EXPECT_EQ(recorder.fragments[1].bytes.size(), FragmentSize + 1);
    EXPECT_EQ(recorder.fragments[2].bytes.size(), 5U + 1);
}

TEST_F(PacketQueueTest, SplitsOneByteOverAFragmentIntoTwo)
{
    const auto payload = payloadOf(FragmentSize + 1, 3);
    ASSERT_TRUE(queue.push(ByteBuf::copyBuffer(payload), ESendType::Reliable));

    ASSERT_EQ(recorder.fragments.size(), 2U);
    EXPECT_EQ(recorder.fragments[0].bytes.front(), std::byte{1});
    EXPECT_EQ(recorder.fragments[1].bytes, (std::vector{std::byte{0}, payload.back()}));
}

TEST_F(PacketQueueTest, ReassemblesWhatItSent)
{
    for (const auto size : {std::size_t{1}, std::size_t{1000}, FragmentSize, FragmentSize + 1, FragmentSize * 3 + 17}) {
        recorder = {};
        const auto payload = payloadOf(size, static_cast<std::uint8_t>(size));
        ASSERT_TRUE(queue.push(ByteBuf::copyBuffer(payload), ESendType::Reliable));
        deliverEverySentFragment();
        ASSERT_EQ(recorder.packets.size(), 1U) << size;
        EXPECT_EQ(recorder.packets[0], payload) << size;
        EXPECT_TRUE(recorder.closed.empty()) << size;
    }
}

TEST_F(PacketQueueTest, DeliversNothingBeforeTheLastFragment)
{
    ASSERT_TRUE(queue.push(ByteBuf::copyBuffer(payloadOf(FragmentSize * 2 + 1, 4)), ESendType::Reliable));
    queue.onOrderedMessage(ByteBuf::copyBuffer(recorder.fragments[0].bytes));
    queue.onOrderedMessage(ByteBuf::copyBuffer(recorder.fragments[1].bytes));
    EXPECT_TRUE(recorder.packets.empty());

    queue.onOrderedMessage(ByteBuf::copyBuffer(recorder.fragments[2].bytes));
    EXPECT_EQ(recorder.packets.size(), 1U);
}

TEST_F(PacketQueueTest, RefusesToFragmentAnUnreliablePacket)
{
    EXPECT_FALSE(queue.push(ByteBuf::copyBuffer(payloadOf(FragmentSize + 1, 5)), ESendType::Unreliable));
    EXPECT_TRUE(recorder.fragments.empty());
}

TEST_F(PacketQueueTest, RefusesAPacketBeyondTheFragmentCount)
{
    ASSERT_TRUE(queue.setMaxMessageSize(3));
    EXPECT_TRUE(queue.push(ByteBuf::copyBuffer(payloadOf(PacketQueue::MAX_FRAGMENTS * 2, 6)), ESendType::Reliable));
    EXPECT_EQ(recorder.fragments.size(), PacketQueue::MAX_FRAGMENTS);

    recorder = {};
    EXPECT_FALSE(
        queue.push(ByteBuf::copyBuffer(payloadOf(PacketQueue::MAX_FRAGMENTS * 2 + 1, 7)), ESendType::Reliable));
    EXPECT_TRUE(recorder.fragments.empty());
}

TEST_F(PacketQueueTest, FragmentsToTheNegotiatedMessageSize)
{
    EXPECT_FALSE(queue.setMaxMessageSize(1));
    ASSERT_TRUE(queue.setMaxMessageSize(5));
    ASSERT_TRUE(queue.push(ByteBuf::copyBuffer(payloadOf(9, 8)), ESendType::Reliable));

    ASSERT_EQ(recorder.fragments.size(), 3U);
    EXPECT_EQ(recorder.fragments[0].bytes.size(), 5U);
    EXPECT_EQ(recorder.fragments[1].bytes.size(), 5U);
    EXPECT_EQ(recorder.fragments[2].bytes.size(), 2U);
}

TEST_F(PacketQueueTest, StripsTheHeaderFromAnUnorderedMessage)
{
    queue.onUnorderedMessage(ByteBuf::copyBuffer(std::vector{std::byte{0}, std::byte{0xAB}}));
    ASSERT_EQ(recorder.packets.size(), 1U);
    EXPECT_EQ(recorder.packets[0], (std::vector{std::byte{0xAB}}));
}

TEST_F(PacketQueueTest, ClosesTheOrderedChannelOnAHeaderOnlyMessage)
{
    queue.onOrderedMessage(ByteBuf::copyBuffer(std::vector{std::byte{0}}));
    EXPECT_TRUE(recorder.packets.empty());
    EXPECT_EQ(recorder.closed, (std::vector{ESendType::Reliable}));
}

TEST_F(PacketQueueTest, ClosesTheUnorderedChannelOnAnEmptyMessage)
{
    queue.onUnorderedMessage(ByteBuf{});
    EXPECT_TRUE(recorder.packets.empty());
    EXPECT_EQ(recorder.closed, (std::vector{ESendType::Unreliable}));
}

TEST_F(PacketQueueTest, ClosesOnAFragmentOutOfCountdown)
{
    queue.onOrderedMessage(ByteBuf::copyBuffer(std::vector{std::byte{2}, std::byte{0x01}}));
    queue.onOrderedMessage(ByteBuf::copyBuffer(std::vector{std::byte{0}, std::byte{0x02}}));
    EXPECT_TRUE(recorder.packets.empty());
    EXPECT_EQ(recorder.closed, (std::vector{ESendType::Reliable}));

    queue.onOrderedMessage(ByteBuf::copyBuffer(std::vector{std::byte{0}, std::byte{0x03}}));
    ASSERT_EQ(recorder.packets.size(), 1U);
    EXPECT_EQ(recorder.packets[0], (std::vector{std::byte{0x03}}));
}

TEST_F(PacketQueueTest, ClosesWhenReassemblyOutgrowsTheBound)
{
    Recorder bounded_recorder;
    PacketQueue bounded{bounded_recorder, 4};
    bounded.onOrderedMessage(ByteBuf::copyBuffer(std::vector{std::byte{1}, std::byte{1}, std::byte{2}, std::byte{3}}));
    bounded.onOrderedMessage(ByteBuf::copyBuffer(std::vector{std::byte{0}, std::byte{4}, std::byte{5}}));

    EXPECT_TRUE(bounded_recorder.packets.empty());
    EXPECT_EQ(bounded_recorder.closed, (std::vector{ESendType::Reliable}));
}

TEST_F(PacketQueueTest, ReadsAMessageSpreadOverSegments)
{
    auto message = ByteBuf::copyBuffer(std::vector{std::byte{0}, std::byte{0x10}});
    message.appendToChain(ByteBuf::copyBuffer(std::vector{std::byte{0x11}, std::byte{0x12}}));
    queue.onOrderedMessage(message);

    ASSERT_EQ(recorder.packets.size(), 1U);
    EXPECT_EQ(recorder.packets[0], (std::vector{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}}));
}

}  // namespace
}  // namespace endgate::nethernet
