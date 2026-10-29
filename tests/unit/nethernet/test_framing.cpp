#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/nethernet/framing.h"

namespace endgate::nethernet {
namespace {

constexpr std::size_t FRAGMENT_SIZE = PacketQueue::DEFAULT_MAX_MESSAGE_SIZE - 1;
constexpr std::size_t MAX_PACKET_SIZE = 8 * 1024 * 1024;

template <typename Buf>
    requires std::derived_from<Buf, AbstractByteBuf>
std::vector<std::byte> bytesOf(const Buf &buf)
{
    const auto bytes = buf.nioBuffer();
    return {bytes.begin(), bytes.end()};
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
    void sendFragment(ESendType type, ByteBuf fragment) override
    {
        fragments.push_back({.type = type, .bytes = bytesOf(fragment)});
    }
    void onPacket(ByteBufView packet) override { packets.push_back(bytesOf(packet)); }
    void closeChannel(ESendType type) override { closed.push_back(type); }

    std::vector<Fragment> fragments;
    std::vector<std::vector<std::byte>> packets;
    std::vector<ESendType> closed;
};

class PacketQueueTest : public testing::Test {
protected:
    void deliverEverySentFragment()
    {
        for (const auto &fragment : recorder_.fragments) {
            const auto message = Unpooled::wrappedBuffer(fragment.bytes);
            if (fragment.type == ESendType::Reliable) {
                queue_.onOrderedMessage(message);
            }
            else {
                queue_.onUnorderedMessage(message);
            }
        }
    }

    Recorder recorder_;
    PacketQueue queue_{recorder_, MAX_PACKET_SIZE};
};

TEST_F(PacketQueueTest, SendsASmallPacketAsOneFragmentWithHeaderZero)
{
    const std::vector payload{std::byte{'A'}};
    ASSERT_TRUE(queue_.push(Unpooled::wrappedBuffer(payload), ESendType::Reliable));

    ASSERT_EQ(recorder_.fragments.size(), 1U);
    EXPECT_EQ(recorder_.fragments[0].type, ESendType::Reliable);
    EXPECT_EQ(recorder_.fragments[0].bytes, (std::vector{std::byte{0x00}, std::byte{'A'}}));
}

TEST_F(PacketQueueTest, SendsAnUnreliablePacketOnTheUnorderedChannel)
{
    const std::vector payload{std::byte{'h'}, std::byte{'i'}};
    ASSERT_TRUE(queue_.push(Unpooled::wrappedBuffer(payload), ESendType::Unreliable));

    ASSERT_EQ(recorder_.fragments.size(), 1U);
    EXPECT_EQ(recorder_.fragments[0].type, ESendType::Unreliable);
    EXPECT_EQ(recorder_.fragments[0].bytes, (std::vector{std::byte{0x00}, std::byte{'h'}, std::byte{'i'}}));
}

TEST_F(PacketQueueTest, KeepsAPacketOfExactlyOneFragmentWhole)
{
    ASSERT_TRUE(queue_.push(Unpooled::wrappedBuffer(payloadOf(FRAGMENT_SIZE, 1)), ESendType::Reliable));

    ASSERT_EQ(recorder_.fragments.size(), 1U);
    EXPECT_EQ(recorder_.fragments[0].bytes.size(), FRAGMENT_SIZE + 1);
    EXPECT_EQ(recorder_.fragments[0].bytes.front(), std::byte{0x00});
}

TEST_F(PacketQueueTest, CountsFragmentsDownToZero)
{
    const auto payload = payloadOf(FRAGMENT_SIZE * 2 + 5, 2);
    ASSERT_TRUE(queue_.push(Unpooled::wrappedBuffer(payload), ESendType::Reliable));

    ASSERT_EQ(recorder_.fragments.size(), 3U);
    EXPECT_EQ(recorder_.fragments[0].bytes.front(), std::byte{2});
    EXPECT_EQ(recorder_.fragments[1].bytes.front(), std::byte{1});
    EXPECT_EQ(recorder_.fragments[2].bytes.front(), std::byte{0});
    EXPECT_EQ(recorder_.fragments[0].bytes.size(), FRAGMENT_SIZE + 1);
    EXPECT_EQ(recorder_.fragments[1].bytes.size(), FRAGMENT_SIZE + 1);
    EXPECT_EQ(recorder_.fragments[2].bytes.size(), 5U + 1);
}

TEST_F(PacketQueueTest, SplitsOneByteOverAFragmentIntoTwo)
{
    const auto payload = payloadOf(FRAGMENT_SIZE + 1, 3);
    ASSERT_TRUE(queue_.push(Unpooled::wrappedBuffer(payload), ESendType::Reliable));

    ASSERT_EQ(recorder_.fragments.size(), 2U);
    EXPECT_EQ(recorder_.fragments[0].bytes.front(), std::byte{1});
    EXPECT_EQ(recorder_.fragments[1].bytes, (std::vector{std::byte{0}, payload.back()}));
}

TEST_F(PacketQueueTest, ReassemblesWhatItSent)
{
    for (const auto size :
         {std::size_t{1}, std::size_t{1000}, FRAGMENT_SIZE, FRAGMENT_SIZE + 1, FRAGMENT_SIZE * 3 + 17}) {
        recorder_ = {};
        const auto payload = payloadOf(size, static_cast<std::uint8_t>(size));
        ASSERT_TRUE(queue_.push(Unpooled::wrappedBuffer(payload), ESendType::Reliable));
        deliverEverySentFragment();
        ASSERT_EQ(recorder_.packets.size(), 1U) << size;
        EXPECT_EQ(recorder_.packets[0], payload) << size;
        EXPECT_TRUE(recorder_.closed.empty()) << size;
    }
}

TEST_F(PacketQueueTest, DeliversNothingBeforeTheLastFragment)
{
    ASSERT_TRUE(queue_.push(Unpooled::wrappedBuffer(payloadOf(FRAGMENT_SIZE * 2 + 1, 4)), ESendType::Reliable));
    queue_.onOrderedMessage(Unpooled::wrappedBuffer(recorder_.fragments[0].bytes));
    queue_.onOrderedMessage(Unpooled::wrappedBuffer(recorder_.fragments[1].bytes));
    EXPECT_TRUE(recorder_.packets.empty());

    queue_.onOrderedMessage(Unpooled::wrappedBuffer(recorder_.fragments[2].bytes));
    EXPECT_EQ(recorder_.packets.size(), 1U);
}

TEST_F(PacketQueueTest, RefusesToFragmentAnUnreliablePacket)
{
    EXPECT_FALSE(queue_.push(Unpooled::wrappedBuffer(payloadOf(FRAGMENT_SIZE + 1, 5)), ESendType::Unreliable));
    EXPECT_TRUE(recorder_.fragments.empty());
}

TEST_F(PacketQueueTest, RefusesAPacketBeyondTheFragmentCount)
{
    ASSERT_TRUE(queue_.setMaxMessageSize(3));
    EXPECT_TRUE(
        queue_.push(Unpooled::wrappedBuffer(payloadOf(PacketQueue::MAX_FRAGMENTS * 2, 6)), ESendType::Reliable));
    EXPECT_EQ(recorder_.fragments.size(), PacketQueue::MAX_FRAGMENTS);

    recorder_ = {};
    EXPECT_FALSE(
        queue_.push(Unpooled::wrappedBuffer(payloadOf(PacketQueue::MAX_FRAGMENTS * 2 + 1, 7)), ESendType::Reliable));
    EXPECT_TRUE(recorder_.fragments.empty());
}

TEST_F(PacketQueueTest, FragmentsToTheNegotiatedMessageSize)
{
    EXPECT_FALSE(queue_.setMaxMessageSize(1));
    ASSERT_TRUE(queue_.setMaxMessageSize(5));
    ASSERT_TRUE(queue_.push(Unpooled::wrappedBuffer(payloadOf(9, 8)), ESendType::Reliable));

    ASSERT_EQ(recorder_.fragments.size(), 3U);
    EXPECT_EQ(recorder_.fragments[0].bytes.size(), 5U);
    EXPECT_EQ(recorder_.fragments[1].bytes.size(), 5U);
    EXPECT_EQ(recorder_.fragments[2].bytes.size(), 2U);
}

TEST_F(PacketQueueTest, StripsTheHeaderFromAnUnorderedMessage)
{
    queue_.onUnorderedMessage(Unpooled::wrappedBuffer(std::vector{std::byte{0}, std::byte{0xAB}}));
    ASSERT_EQ(recorder_.packets.size(), 1U);
    EXPECT_EQ(recorder_.packets[0], (std::vector{std::byte{0xAB}}));
}

TEST_F(PacketQueueTest, ClosesTheOrderedChannelOnAHeaderOnlyMessage)
{
    queue_.onOrderedMessage(Unpooled::wrappedBuffer(std::vector{std::byte{0}}));
    EXPECT_TRUE(recorder_.packets.empty());
    EXPECT_EQ(recorder_.closed, (std::vector{ESendType::Reliable}));
}

TEST_F(PacketQueueTest, ClosesTheUnorderedChannelOnAnEmptyMessage)
{
    queue_.onUnorderedMessage(ByteBufView{});
    EXPECT_TRUE(recorder_.packets.empty());
    EXPECT_EQ(recorder_.closed, (std::vector{ESendType::Unreliable}));
}

TEST_F(PacketQueueTest, ClosesOnAFragmentOutOfCountdown)
{
    queue_.onOrderedMessage(Unpooled::wrappedBuffer(std::vector{std::byte{2}, std::byte{0x01}}));
    queue_.onOrderedMessage(Unpooled::wrappedBuffer(std::vector{std::byte{0}, std::byte{0x02}}));
    EXPECT_TRUE(recorder_.packets.empty());
    EXPECT_EQ(recorder_.closed, (std::vector{ESendType::Reliable}));

    queue_.onOrderedMessage(Unpooled::wrappedBuffer(std::vector{std::byte{0}, std::byte{0x03}}));
    ASSERT_EQ(recorder_.packets.size(), 1U);
    EXPECT_EQ(recorder_.packets[0], (std::vector{std::byte{0x03}}));
}

TEST_F(PacketQueueTest, ClosesWhenReassemblyOutgrowsTheBound)
{
    Recorder bounded_recorder;
    PacketQueue bounded{bounded_recorder, 4};
    bounded.onOrderedMessage(
        Unpooled::wrappedBuffer(std::vector{std::byte{1}, std::byte{1}, std::byte{2}, std::byte{3}}));
    bounded.onOrderedMessage(Unpooled::wrappedBuffer(std::vector{std::byte{0}, std::byte{4}, std::byte{5}}));

    EXPECT_TRUE(bounded_recorder.packets.empty());
    EXPECT_EQ(bounded_recorder.closed, (std::vector{ESendType::Reliable}));
}

TEST_F(PacketQueueTest, ReadsAMessageOfSeveralBytes)
{
    const std::vector message{std::byte{0}, std::byte{0x10}, std::byte{0x11}, std::byte{0x12}};
    queue_.onOrderedMessage(Unpooled::wrappedBuffer(message));

    ASSERT_EQ(recorder_.packets.size(), 1U);
    EXPECT_EQ(recorder_.packets[0], (std::vector{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}}));
}

}  // namespace
}  // namespace endgate::nethernet
