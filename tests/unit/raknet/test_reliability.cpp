#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/network/raknet/reliability.h"
#include "simulation.h"
#include "support.h"

namespace endgate::raknet {
namespace {

using namespace std::chrono_literals;
using test::fromHex;
using test::Simulation;

class Peer : public ReliabilityLayer::Listener {
public:
    Peer(Simulation &simulation, std::size_t index, std::size_t mtu = 1400)
        : simulation_(simulation), index_(index), layer(*this, mtu, simulation.now())
    {
        simulation.attach(index, {.receive =
                                      [this](std::span<const std::byte> datagram, TimePoint now) {
                                          layer.handleSocketReceiveFromConnectedPlayer(datagram, now);
                                          drain();
                                          layer.update(now);
                                          drain();
                                      },
                                  .next_timeout = [this] { return layer.nextTimeout(); },
                                  .timeout =
                                      [this](TimePoint now) {
                                          layer.update(now);
                                          drain();
                                      }});
    }

    void sendDatagram(std::span<const std::byte> datagram) override
    {
        datagrams.emplace_back(datagram.begin(), datagram.end());
        simulation_.transmit(index_, datagram);
    }

    void onReliabilityLayerNotification(std::string_view message, bool is_error) override
    {
        notifications.emplace_back(message);
        errors += is_error ? 1 : 0;
    }

    void send(const std::vector<std::byte> &message, PacketReliability reliability, std::uint8_t channel = 0,
              std::uint32_t receipt = 0, PacketPriority priority = PacketPriority::High)
    {
        layer.send(LegacyByteBuf::copyBuffer(message), priority, reliability, channel, receipt, simulation_.now());
        layer.update(simulation_.now());
        drain();
    }

    void inject(std::string_view hex)
    {
        const auto bytes = fromHex(hex);
        layer.handleSocketReceiveFromConnectedPlayer(bytes, simulation_.now());
        drain();
        layer.update(simulation_.now());
        drain();
    }

    void drain()
    {
        while (auto message = layer.receive()) {
            std::vector<std::byte> bytes(message->length());
            message->copyTo(bytes);
            received.push_back(std::move(bytes));
        }
    }

    ReliabilityLayer layer;
    std::vector<std::vector<std::byte>> received;
    std::vector<std::vector<std::byte>> datagrams;
    std::vector<std::string> notifications;
    int errors{0};

private:
    Simulation &simulation_;
    std::size_t index_;
};

std::vector<std::byte> message(std::uint32_t sequence, std::size_t size = 16, std::uint8_t tag = 0)
{
    std::vector<std::byte> bytes(std::max<std::size_t>(size, 6));
    bytes[0] = std::byte{0xFE};
    bytes[1] = std::byte{tag};
    std::memcpy(bytes.data() + 2, &sequence, sizeof(sequence));
    for (std::size_t i = 6; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((i * 31 + sequence) & 0xFF);
    }
    return bytes;
}

std::uint32_t sequenceOf(const std::vector<std::byte> &bytes)
{
    std::uint32_t sequence = 0;
    std::memcpy(&sequence, bytes.data() + 2, sizeof(sequence));
    return sequence;
}

struct Pair {
    explicit Pair(std::uint32_t seed = 1, std::size_t mtu = 1400)
        : simulation(seed), a(simulation, 0, mtu), b(simulation, 1, mtu)
    {
    }

    void lossy(double loss, double duplicate, double reorder)
    {
        simulation.setConditions(0, {.loss = loss, .duplicate = duplicate, .reorder = reorder});
        simulation.setConditions(1, {.loss = loss, .duplicate = duplicate, .reorder = reorder});
    }

    Simulation simulation;
    Peer a;
    Peer b;
};

TEST(Reliability, ReliableOrderedArrivesInOrderOnACleanLink)
{
    Pair pair;
    for (std::uint32_t i = 0; i < 200; ++i) {
        pair.a.send(message(i, 100), PacketReliability::ReliableOrdered);
    }
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.b.received.size() == 200; }, 10s));
    for (std::uint32_t i = 0; i < 200; ++i) {
        EXPECT_EQ(pair.b.received[i], message(i, 100));
    }
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.a.layer.getMessagesInResendBuffer() == 0; }, 10s));
    EXPECT_EQ(pair.a.errors + pair.b.errors, 0);
}

class EveryReliability : public testing::TestWithParam<PacketReliability> {};

TEST_P(EveryReliability, CleanLinkDeliversEverything)
{
    Pair pair;
    for (std::uint32_t i = 0; i < 100; ++i) {
        pair.a.send(message(i), GetParam(), 3, i + 1);
    }
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.b.received.size() == 100; }, 10s));
    for (std::uint32_t i = 0; i < 100; ++i) {
        EXPECT_EQ(pair.b.received[i], message(i));
    }
}

TEST_P(EveryReliability, LossyLinkKeepsTheGuaranteesOfTheReliability)
{
    const auto reliability = GetParam();
    Pair pair{7};
    pair.lossy(0.2, 0.1, 0.2);
    constexpr std::uint32_t count = 300;
    for (std::uint32_t i = 0; i < count; ++i) {
        pair.a.send(message(i, 200), reliability, 1, i + 1);
        pair.simulation.runUntil([] { return false; }, 1ms);
    }
    const bool reliable = reliability == PacketReliability::Reliable ||
                          reliability == PacketReliability::ReliableOrdered ||
                          reliability == PacketReliability::ReliableSequenced ||
                          reliability == PacketReliability::ReliableWithAckReceipt ||
                          reliability == PacketReliability::ReliableOrderedWithAckReceipt;
    pair.simulation.runUntil([&] { return pair.a.layer.getMessagesInResendBuffer() == 0 && reliable; }, 60s);

    std::vector<std::uint32_t> sequences;
    for (const auto &received : pair.b.received) {
        ASSERT_EQ(received, message(sequenceOf(received), 200));
        sequences.push_back(sequenceOf(received));
    }
    const std::set<std::uint32_t> unique(sequences.begin(), sequences.end());
    if (reliable || reliability == PacketReliability::UnreliableSequenced) {
        EXPECT_EQ(unique.size(), sequences.size()) << "a message was delivered twice";
    }

    const bool ordered = reliability == PacketReliability::ReliableOrdered ||
                         reliability == PacketReliability::ReliableOrderedWithAckReceipt;
    const bool sequenced =
        reliability == PacketReliability::ReliableSequenced || reliability == PacketReliability::UnreliableSequenced;
    if (ordered || sequenced) {
        EXPECT_TRUE(std::ranges::is_sorted(sequences));
    }
    if (reliable && !sequenced) {
        EXPECT_EQ(sequences.size(), count);
    }
    if (sequenced && reliable) {
        ASSERT_FALSE(sequences.empty());
        EXPECT_EQ(sequences.back(), count - 1);
    }
    EXPECT_EQ(pair.b.errors, 0);
}

INSTANTIATE_TEST_SUITE_P(Reliability, EveryReliability,
                         testing::Values(PacketReliability::Unreliable, PacketReliability::UnreliableSequenced,
                                         PacketReliability::Reliable, PacketReliability::ReliableOrdered,
                                         PacketReliability::ReliableSequenced,
                                         PacketReliability::UnreliableWithAckReceipt,
                                         PacketReliability::ReliableWithAckReceipt,
                                         PacketReliability::ReliableOrderedWithAckReceipt));

TEST(Reliability, ReceiptsAreReportedForAckedMessages)
{
    Pair pair;
    for (std::uint32_t i = 0; i < 10; ++i) {
        pair.a.send(message(i), PacketReliability::ReliableWithAckReceipt, 0, 100 + i);
    }
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.a.received.size() == 10; }, 10s));
    std::set<std::uint32_t> serials;
    for (const auto &receipt : pair.a.received) {
        ASSERT_EQ(receipt.size(), 5U);
        EXPECT_EQ(receipt[0], std::byte{static_cast<std::uint8_t>(MessageId::SndReceiptAcked)});
        std::uint32_t serial = 0;
        std::memcpy(&serial, receipt.data() + 1, sizeof(serial));
        serials.insert(serial);
    }
    EXPECT_EQ(serials.size(), 10U);
    EXPECT_EQ(*serials.begin(), 100U);
}

TEST(Reliability, UnreliableReceiptsReportLossWhenNothingAcks)
{
    Pair pair;
    pair.simulation.setConditions(0, {.loss = 1.0});
    pair.a.send(message(0), PacketReliability::UnreliableWithAckReceipt, 0, 55);
    ASSERT_TRUE(pair.simulation.runUntil([&] { return !pair.a.received.empty(); }, 5s));
    EXPECT_EQ(pair.a.received[0][0], std::byte{static_cast<std::uint8_t>(MessageId::SndReceiptLoss)});
}

TEST(Reliability, ChannelsAreOrderedIndependently)
{
    Pair pair{3};
    pair.lossy(0.1, 0, 0.3);
    for (std::uint32_t i = 0; i < 320; ++i) {
        pair.a.send(message(i, 50, static_cast<std::uint8_t>(i % 32)), PacketReliability::ReliableOrdered,
                    static_cast<std::uint8_t>(i % 32));
    }
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.b.received.size() == 320; }, 60s));
    std::map<std::uint8_t, std::vector<std::uint32_t>> per_channel;
    for (const auto &received : pair.b.received) {
        per_channel[std::to_integer<std::uint8_t>(received[1])].push_back(sequenceOf(received));
    }
    ASSERT_EQ(per_channel.size(), 32U);
    for (const auto &[channel, sequences] : per_channel) {
        EXPECT_TRUE(std::ranges::is_sorted(sequences)) << int{channel};
        EXPECT_EQ(sequences.size(), 10U);
    }
}

class LargeMessages : public testing::TestWithParam<std::size_t> {};

TEST_P(LargeMessages, SplitsReassembleUnderLossDuplicationAndReordering)
{
    Pair pair{11};
    pair.lossy(0.1, 0.05, 0.2);
    const auto large = message(9, GetParam());
    pair.a.send(large, PacketReliability::ReliableOrdered);
    pair.a.send(message(10, 40), PacketReliability::ReliableOrdered);
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.b.received.size() == 2; }, 120s));
    EXPECT_EQ(pair.b.received[0], large);
    EXPECT_EQ(pair.b.received[1], message(10, 40));
    EXPECT_EQ(pair.b.errors, 0);
    EXPECT_LE(pair.simulation.largest(0), 1400 - UDP_HEADER_SIZE);
}

INSTANTIATE_TEST_SUITE_P(Reliability, LargeMessages, testing::Values(1500, 65535, 300000, 1 << 20, 2700000));

TEST(Reliability, UnreliableMessagesThatMustSplitBecomeReliable)
{
    Pair pair{5};
    pair.lossy(0.2, 0, 0);
    pair.a.send(message(1, 5000), PacketReliability::Unreliable);
    pair.a.send(message(2, 5000), PacketReliability::UnreliableSequenced);
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.b.received.size() == 2; }, 30s));
    EXPECT_EQ(pair.b.received[0], message(1, 5000));
    EXPECT_EQ(pair.b.received[1], message(2, 5000));
}

TEST(Reliability, DatagramsNeverExceedTheMtu)
{
    for (const std::size_t mtu : {576, 1200, 1400, 1600}) {
        Pair pair{1, mtu};
        for (std::uint32_t i = 0; i < 50; ++i) {
            pair.a.send(message(i, 37 * i + 1), PacketReliability::ReliableOrdered);
        }
        ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.b.received.size() == 50; }, 10s));
        EXPECT_LE(pair.simulation.largest(0), mtu - UDP_HEADER_SIZE) << mtu;
        EXPECT_LE(pair.simulation.largest(1), mtu - UDP_HEADER_SIZE) << mtu;
    }
}

TEST(Reliability, StaleUnreliableMessagesAreCulledEvenWhileTheWindowIsClosed)
{
    Pair pair;
    pair.a.layer.setUnreliableTimeout(1000ms);
    pair.simulation.setConditions(0, {.loss = 1.0});
    for (std::uint32_t i = 0; i < 4; ++i) {
        pair.a.send(message(i, 1300), PacketReliability::Reliable);
    }
    for (std::uint32_t i = 100; i < 200; ++i) {
        pair.a.send(message(i, 100), PacketReliability::Unreliable);
    }
    EXPECT_EQ(pair.a.layer.getMessagesInSendBuffer(), 102U);
    pair.simulation.runUntil([] { return false; }, 1600ms);
    EXPECT_EQ(pair.a.layer.getMessagesInSendBuffer(), 2U);
    pair.simulation.setConditions(0, {});
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.a.layer.getMessagesInResendBuffer() == 0; }, 10s));
    pair.simulation.runUntil([] { return false; }, 100ms);
    std::set<std::uint32_t> delivered;
    for (const auto &received : pair.b.received) {
        delivered.insert(sequenceOf(received));
    }
    EXPECT_EQ(delivered, (std::set<std::uint32_t>{0, 1, 2, 3}));
}

TEST(Reliability, SmallMessagesShareADatagram)
{
    Pair pair;
    pair.simulation.setConditions(0, {.loss = 1.0});
    for (std::uint32_t i = 0; i < 20; ++i) {
        pair.a.layer.send(LegacyByteBuf::copyBuffer(message(i)), PacketPriority::High, PacketReliability::Reliable, 0,
                          0, pair.simulation.now());
    }
    pair.a.layer.update(pair.simulation.now());
    EXPECT_EQ(pair.a.datagrams.size(), 1U);
}

TEST(Reliability, ImmediatePriorityOvertakesQueuedHighPriority)
{
    Pair pair;
    for (std::uint32_t i = 0; i < 4; ++i) {
        pair.a.layer.send(LegacyByteBuf::copyBuffer(message(i)), PacketPriority::Low, PacketReliability::Reliable, 0, 0,
                          pair.simulation.now());
    }
    pair.a.layer.send(LegacyByteBuf::copyBuffer(message(99)), PacketPriority::Immediate, PacketReliability::Reliable, 0,
                      0, pair.simulation.now());
    pair.a.layer.update(pair.simulation.now());
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.b.received.size() == 5; }, 5s));
    EXPECT_EQ(sequenceOf(pair.b.received[0]), 99U);
    EXPECT_EQ(sequenceOf(pair.b.received[1]), 0U);
    EXPECT_EQ(sequenceOf(pair.b.received[4]), 3U);
}

TEST(Reliability, AckIsDelayedOnceTheRoundTripIsKnown)
{
    Pair pair;
    pair.a.send(message(0), PacketReliability::Reliable);
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.a.layer.getMessagesInResendBuffer() == 0; }, 5s));
    pair.b.send(message(1), PacketReliability::Reliable);
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.b.layer.getMessagesInResendBuffer() == 0; }, 5s));

    const auto before = pair.b.datagrams.size();
    pair.a.send(message(2), PacketReliability::Reliable);
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.b.received.size() == 2; }, 5s));
    EXPECT_EQ(pair.b.datagrams.size(), before);
    const auto deadline = pair.b.layer.nextTimeout();
    ASSERT_TRUE(deadline);
    EXPECT_GT(*deadline, pair.simulation.now());
    EXPECT_LE(*deadline - pair.simulation.now(), 10ms);
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.b.datagrams.size() > before; }, 20ms));
}

TEST(Reliability, UnacknowledgedMessagesAreResentAfterTheRto)
{
    Pair pair;
    pair.simulation.setConditions(1, {.loss = 1.0});
    pair.a.send(message(0), PacketReliability::Reliable);
    const auto first = pair.simulation.now();
    ASSERT_EQ(pair.a.datagrams.size(), 1U);
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.a.datagrams.size() == 2; }, 3s));
    EXPECT_EQ(pair.simulation.now() - first, 2s);
    EXPECT_EQ(pair.b.received.size(), 1U);
}

TEST(Reliability, NakTriggersImmediateResend)
{
    Pair pair;
    pair.a.send(message(0), PacketReliability::Reliable);
    pair.a.send(message(1), PacketReliability::Reliable);
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.a.layer.getMessagesInResendBuffer() == 0; }, 5s));
    pair.simulation.setConditions(1, {.loss = 1.0});
    pair.a.send(message(2), PacketReliability::Reliable);
    const auto sent = pair.a.datagrams.size();
    pair.a.inject("a0000101"
                  "020000");
    EXPECT_EQ(pair.a.datagrams.size(), sent + 1);
}

TEST(Reliability, SilentPeerMakesTheConnectionDead)
{
    Pair pair;
    pair.simulation.setConditions(1, {.loss = 1.0});
    pair.a.send(message(0), PacketReliability::Reliable);
    const auto start = pair.simulation.now();
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.a.layer.isDeadConnection(); }, 20s));
    EXPECT_GE(pair.simulation.now() - start, 10s);
    EXPECT_LT(pair.simulation.now() - start, 10s + 10ms);
}

TEST(Reliability, PeerThatKeepsSendingButNeverAcksIsDroppedAfterTheTimeout)
{
    Pair pair;
    pair.simulation.setConditions(1, {.loss = 1.0});
    pair.a.send(message(0), PacketReliability::Reliable);
    const auto start = pair.simulation.now();
    for (std::uint32_t i = 0; i < 40 && !pair.a.layer.isDeadConnection(); ++i) {
        pair.simulation.runUntil([] { return false; }, 1s);
        const auto number = test::toHex(std::array{std::byte(i), std::byte{0}, std::byte{0}});
        pair.a.inject("84" + number +
                      "000008"
                      "fe");
    }
    EXPECT_TRUE(pair.a.layer.isDeadConnection());
    EXPECT_GE(pair.simulation.now() - start, 10s);
    EXPECT_LE(pair.simulation.now() - start, 13s);
}

TEST(Reliability, SkippedDatagramsAreNaked)
{
    Pair pair;
    pair.simulation.setConditions(1, {.loss = 1.0});
    pair.b.inject("84000000"
                  "000018fe0102");
    pair.b.inject("84030000"
                  "000018fe0102");
    ASSERT_GE(pair.b.datagrams.size(), 2U);
    EXPECT_EQ(test::toHex(pair.b.datagrams.back()), "a0"
                                                    "0001"
                                                    "00010000020000");
}

TEST(Reliability, DuplicateReliableMessagesAreDeliveredOnce)
{
    Pair pair;
    pair.b.inject("84000000"
                  "4000180000"
                  "00fe0102");
    pair.b.inject("84010000"
                  "4000180000"
                  "00fe0102");
    pair.b.inject("84020000"
                  "4000180200"
                  "00fe0103");
    pair.b.inject("84030000"
                  "4000180100"
                  "00fe0104");
    pair.b.inject("84040000"
                  "4000180200"
                  "00fe0103");
    ASSERT_EQ(pair.b.received.size(), 3U);
    EXPECT_EQ(test::toHex(pair.b.received[2]), "fe0104");
}

TEST(Reliability, OrderedMessagesWaitForTheGap)
{
    Pair pair;
    pair.b.inject("84000000"
                  "600018"
                  "010000"
                  "010000"
                  "00"
                  "fe0101");
    EXPECT_TRUE(pair.b.received.empty());
    pair.b.inject("84010000"
                  "600018"
                  "000000"
                  "000000"
                  "00"
                  "fe0100");
    ASSERT_EQ(pair.b.received.size(), 2U);
    EXPECT_EQ(test::toHex(pair.b.received[0]), "fe0100");
    EXPECT_EQ(test::toHex(pair.b.received[1]), "fe0101");
}

TEST(Reliability, OlderSequencedMessagesAreDropped)
{
    Pair pair;
    pair.b.inject("84000000"
                  "200018020000000000"
                  "05fe0102");
    pair.b.inject("84010000"
                  "200018000000000000"
                  "05fe0100");
    pair.b.inject("84020000"
                  "200018030000000000"
                  "05fe0103");
    ASSERT_EQ(pair.b.received.size(), 2U);
    EXPECT_EQ(test::toHex(pair.b.received[0]), "fe0102");
    EXPECT_EQ(test::toHex(pair.b.received[1]), "fe0103");
}

TEST(Reliability, SplitPartsReassembleInIndexOrderWhateverTheirArrival)
{
    Pair pair;
    const std::array<std::string_view, 3> parts{"fe00", "0102", "0304"};
    for (const std::uint32_t index : {2U, 0U, 1U}) {
        const auto datagram = std::string{"84"} +
                              test::toHex(std::array{std::byte(index), std::byte{0}, std::byte{0}}) + "700010" +
                              test::toHex(std::array{std::byte(index), std::byte{0}, std::byte{0}}) +
                              "000000"
                              "00"
                              "00000003"
                              "0007"
                              "0000000" +
                              std::to_string(index) + std::string{parts[index]};
        pair.b.inject(datagram);
    }
    ASSERT_EQ(pair.b.received.size(), 1U);
    EXPECT_EQ(test::toHex(pair.b.received[0]), "fe0001020304");
}

TEST(Reliability, RepeatedUnreliableSplitPartsAreIgnored)
{
    Pair pair;
    pair.b.inject("84000000"
                  "100010"
                  "00000002"
                  "0001"
                  "00000000"
                  "fe00");
    pair.b.inject("84010000"
                  "100010"
                  "00000002"
                  "0001"
                  "00000000"
                  "fe00");
    EXPECT_TRUE(pair.b.received.empty());
    pair.b.inject("84020000"
                  "100010"
                  "00000002"
                  "0001"
                  "00000001"
                  "0102");
    ASSERT_EQ(pair.b.received.size(), 1U);
    EXPECT_EQ(test::toHex(pair.b.received[0]), "fe000102");
}

TEST(Reliability, SplitBoundsAreEnforced)
{
    Pair pair;
    pair.b.inject("84000000"
                  "100010"
                  "00000801"
                  "0001"
                  "00000000"
                  "fe00");
    EXPECT_EQ(pair.b.errors, 1);
    pair.b.inject("84010000"
                  "100010"
                  "00000800"
                  "0001"
                  "00000000"
                  "fe00");
    EXPECT_EQ(pair.b.errors, 1);
    for (std::uint32_t id = 2; id <= 256; ++id) {
        const auto split_id = test::toHex(std::array{std::byte(id >> 8U), std::byte(id & 0xFFU)});
        const auto number = test::toHex(std::array{std::byte(id & 0xFFU), std::byte(id >> 8U), std::byte{0}});
        pair.b.inject("84" + number +
                      "100010"
                      "00000002" +
                      split_id +
                      "00000000"
                      "fe00");
    }
    EXPECT_EQ(pair.b.errors, 1);
    pair.b.inject("84010100"
                  "100010"
                  "00000002"
                  "0101"
                  "00000000"
                  "fe00");
    EXPECT_EQ(pair.b.errors, 2);
    pair.b.inject("84020100"
                  "100010"
                  "00000003"
                  "0001"
                  "00000001"
                  "fe00");
    EXPECT_EQ(pair.b.errors, 3);
}

TEST(Reliability, HeldSplitFragmentsAreChargedTheirOverheadAndBounded)
{
    Pair pair;
    pair.b.layer.setMaximumHeldBytes(10 * ReliabilityLayer::HELD_PACKET_OVERHEAD);
    for (std::uint32_t id = 0; id < 9; ++id) {
        const auto number = test::toHex(std::array{std::byte(id), std::byte{0}, std::byte{0}});
        const auto split_id = test::toHex(std::array{std::byte{0}, std::byte(id)});
        pair.b.inject("84" + number +
                      "100008"
                      "00000800" +
                      split_id +
                      "00000000"
                      "fe");
    }
    EXPECT_EQ(pair.b.errors, 0);
    EXPECT_EQ(pair.b.layer.getHeldBytes(), 9 * (1 + ReliabilityLayer::HELD_PACKET_OVERHEAD));
    pair.b.inject("84090000"
                  "100008"
                  "00000800"
                  "0009"
                  "00000000"
                  "fe");
    EXPECT_EQ(pair.b.errors, 1);
    EXPECT_TRUE(pair.b.layer.isDeadConnection());
}

TEST(Reliability, HeldOrderedPacketsAreBounded)
{
    Pair pair;
    pair.b.layer.setMaximumHeldBytes(4 * (3 + ReliabilityLayer::HELD_PACKET_OVERHEAD));
    for (std::uint32_t i = 0; i < 4; ++i) {
        const auto index = test::toHex(std::array{std::byte(i + 1), std::byte{0}, std::byte{0}});
        pair.b.inject("84" + index + "600018" + index + index +
                      "00"
                      "fe0102");
    }
    EXPECT_FALSE(pair.b.layer.isDeadConnection());
    pair.b.inject("84050000"
                  "600018"
                  "050000"
                  "050000"
                  "00"
                  "fe0102");
    EXPECT_TRUE(pair.b.layer.isDeadConnection());
    EXPECT_TRUE(pair.b.received.empty());
}

TEST(Reliability, ReleasedPacketsGiveTheirBudgetBack)
{
    Pair pair;
    for (std::uint32_t i = 0; i < 3; ++i) {
        const auto index = test::toHex(std::array{std::byte(i + 1), std::byte{0}, std::byte{0}});
        pair.b.inject("84" + index + "600018" + index + index +
                      "00"
                      "fe0102");
    }
    pair.b.inject("84000000"
                  "100010"
                  "00000002"
                  "0001"
                  "00000000"
                  "fe00");
    EXPECT_GT(pair.b.layer.getHeldBytes(), 0U);
    pair.b.inject("84040000"
                  "100010"
                  "00000002"
                  "0001"
                  "00000001"
                  "0102");
    pair.b.inject("84050000"
                  "600018"
                  "000000"
                  "000000"
                  "00"
                  "fe0102");
    EXPECT_EQ(pair.b.received.size(), 5U);
    EXPECT_EQ(pair.b.layer.getHeldBytes(), 0U);
}

TEST(Reliability, HoleCountBeyondTheBoundIsRejected)
{
    Pair pair;
    pair.b.inject("84000000"
                  "400018"
                  "41420f"
                  "fe0102");
    EXPECT_EQ(pair.b.errors, 1);
    EXPECT_TRUE(pair.b.received.empty());
    pair.b.inject("84010000"
                  "400018"
                  "40420f"
                  "fe0102");
    EXPECT_EQ(pair.b.errors, 1);
    EXPECT_EQ(pair.b.received.size(), 1U);
}

TEST(Reliability, AcknowledgementBoundsMatchTheMojangPatch)
{
    Pair pair;
    std::string too_many_ranges = "c00401";
    for (std::uint32_t i = 0; i < 0x401; ++i) {
        too_many_ranges += "01" + test::toHex(std::array{std::byte(i * 2 & 0xFF), std::byte(i * 2 >> 8), std::byte{0}});
    }
    pair.a.inject(too_many_ranges);
    pair.a.inject("c0"
                  "0001"
                  "00"
                  "000000"
                  "004000");
    pair.a.inject("c0"
                  "0001"
                  "00"
                  "000000"
                  "ffffff");
    pair.a.inject("c0"
                  "0001"
                  "00"
                  "050000"
                  "020000");
    pair.a.inject("c0"
                  "0003"
                  "01"
                  "010000");
    pair.a.inject("a0"
                  "0001"
                  "00"
                  "000000"
                  "004000");
    pair.a.inject("a0"
                  "0001"
                  "00"
                  "000000"
                  "ffffff");
    EXPECT_EQ(pair.a.notifications,
              (std::vector<std::string>{"number of ACK ranges in this datagram exceeds the maximum allowed",
                                        "datagram contains too many ACKs, exceeding MAX_TOTAL_ACKS_PER_DATAGRAM",
                                        "incomingAcks minIndex > maxIndex or maxIndex is too large",
                                        "incomingAcks.Deserialize failed", "incomingAcks.Deserialize failed",
                                        "datagram contains too many NAKs, exceeding MAX_TOTAL_NAKS_PER_DATAGRAM",
                                        "incomingNAKs minIndex>maxIndex or maxIndex is too large"}));
    pair.a.inject("c0"
                  "0001"
                  "00"
                  "000000"
                  "ff3f00");
    EXPECT_EQ(pair.a.notifications.size(), 7U);
}

TEST(Reliability, MalformedDatagramsAreReportedAndDropped)
{
    Pair pair;
    pair.b.inject("8400");
    pair.b.inject("4000000000");
    pair.b.inject("84000000");
    pair.b.inject("84010000"
                  "60001800000000"
                  "20fe0102");
    pair.b.inject("84020000"
                  "000000fe");
    pair.b.inject("84"
                  "60ea00"
                  "000018fe0102");
    EXPECT_EQ(pair.b.notifications, (std::vector<std::string>{"length <= 2 || buffer == 0", "dhf.isValid==false",
                                                              "CreateInternalPacketFromBitStream failed",
                                                              "CreateInternalPacketFromBitStream failed",
                                                              "CreateInternalPacketFromBitStream failed",
                                                              "congestionManager.OnGotPacket failed"}));
    EXPECT_TRUE(pair.b.received.empty());
}

TEST(Reliability, TrailingBytesShorterThanAFrameAreIgnored)
{
    Pair pair;
    pair.b.inject("84000000"
                  "000018fe0102"
                  "000018");
    EXPECT_EQ(pair.b.received.size(), 1U);
    EXPECT_TRUE(pair.b.notifications.empty());
}

TEST(Reliability, ThroughputIsNotCappedByTheTimers)
{
    Pair pair;
    constexpr std::uint32_t count = 4000;
    for (std::uint32_t i = 0; i < count; ++i) {
        pair.a.send(message(i, 1025), PacketReliability::ReliableOrdered);
    }
    const auto start = pair.simulation.now();
    ASSERT_TRUE(pair.simulation.runUntil([&] { return pair.b.received.size() == count; }, 60s));
    EXPECT_LT(pair.simulation.now() - start, 2s);
}

}  // namespace
}  // namespace endgate::raknet
