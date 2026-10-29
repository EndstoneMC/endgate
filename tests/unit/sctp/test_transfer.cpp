#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "network.h"

namespace endgate::rtc::sctp {
namespace {

using namespace std::chrono_literals;
using State = Association::State;

LegacyByteBuf patterned(std::size_t size, std::uint32_t seed)
{
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>((seed * 31 + i * 7) & 0xFFU);
    }
    return LegacyByteBuf::copyBuffer(data);
}

bool intact(const LegacyByteBuf &payload, std::size_t size, std::uint32_t seed)
{
    if (payload.length() != size) {
        return false;
    }
    std::vector<std::byte> data(size);
    payload.copyTo(data);
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] != static_cast<std::byte>((seed * 31 + i * 7) & 0xFFU)) {
            return false;
        }
    }
    return true;
}

std::function<void(std::span<std::byte>)> scripted(std::vector<std::byte> prefix, std::uint32_t seed)
{
    return [prefix = std::move(prefix), rest = seeded(seed), used = std::size_t{0}](std::span<std::byte> out) mutable {
        for (auto &byte : out) {
            if (used < prefix.size()) {
                byte = prefix[used++];
            }
            else {
                rest(std::span{&byte, 1});
            }
        }
    };
}

void connect(Network &network)
{
    network.a->connect(network.now);
    network.b->connect(network.now);
    ASSERT_TRUE(network.runUntil(
        [&] { return network.a->state() == State::Established && network.b->state() == State::Established; }, 10s));
}

class Transfer : public testing::TestWithParam<std::uint32_t> {};

TEST_P(Transfer, DeliversLargeMessagesThroughLossDuplicationAndReordering)
{
    Network network{{}, {}, GetParam()};
    network.impairment = {.loss = 0.05, .duplication = 0.03, .reordering = 0.1};
    connect(network);
    constexpr std::size_t size = 262144;
    for (std::uint32_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(network.a->send(static_cast<std::uint16_t>(i % 2), 53, patterned(size, i), {}, network.now));
    }
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.messages.size() == 8; }, 120s));
    std::uint32_t next[2] = {0, 1};
    for (const auto &message : network.endpoint_b.messages) {
        EXPECT_TRUE(intact(message.payload, size, next[message.stream]));
        next[message.stream] += 2;
    }
    EXPECT_FALSE(network.endpoint_a.closed);
    EXPECT_FALSE(network.endpoint_b.closed);
}

TEST_P(Transfer, KeepsOrderedStreamsInOrderBesideUnorderedTraffic)
{
    Network network{{}, {}, GetParam()};
    network.impairment = {.loss = 0.1, .duplication = 0.05, .reordering = 0.3};
    connect(network);
    constexpr std::uint32_t count = 300;
    for (std::uint32_t i = 0; i < count; ++i) {
        ASSERT_TRUE(network.a->send(0, 51, bufferOf("o" + std::to_string(i)), {}, network.now));
        ASSERT_TRUE(network.a->send(1, 51, bufferOf("u" + std::to_string(i)), {.unordered = true}, network.now));
        ASSERT_TRUE(network.a->send(2, 53, patterned(3000 + i, i), {}, network.now));
        network.run(2ms);
    }
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.messages.size() == 3 * count; }, 120s));

    std::uint32_t ordered = 0;
    std::uint32_t fragmented = 0;
    std::set<std::string> unordered;
    for (const auto &message : network.endpoint_b.messages) {
        if (message.stream == 0) {
            EXPECT_EQ(text(message.payload), "o" + std::to_string(ordered++));
            EXPECT_FALSE(message.unordered);
        }
        else if (message.stream == 1) {
            EXPECT_TRUE(unordered.insert(text(message.payload)).second);
            EXPECT_TRUE(message.unordered);
        }
        else {
            EXPECT_TRUE(intact(message.payload, 3000 + fragmented, fragmented));
            ++fragmented;
        }
    }
    EXPECT_EQ(unordered.size(), count);
}

TEST_P(Transfer, AbandonsUnreliableMessagesWithoutStallingReliableOnes)
{
    Network network{{}, {}, GetParam()};
    network.impairment = {.loss = 0.1, .duplication = 0.02, .reordering = 0.1};
    connect(network);
    auto forward_tsns = 0;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        forward_tsns += to_b && hasChunk(packet, ChunkType::ForwardTsn) ? 1 : 0;
        return true;
    };
    constexpr std::uint32_t count = 200;
    const Sender::SendOptions unreliable{.unordered = true, .max_retransmits = 0};
    for (std::uint32_t i = 0; i < count; ++i) {
        ASSERT_TRUE(network.a->send(1, 53, bufferOf("u" + std::to_string(i)), unreliable, network.now));
        ASSERT_TRUE(network.a->send(0, 53, bufferOf("r" + std::to_string(i)), {}, network.now));
        network.run(5ms);
    }
    const auto reliable = [&] {
        return std::ranges::count_if(network.endpoint_b.messages,
                                     [](const Message &message) { return message.stream == 0; });
    };
    ASSERT_TRUE(network.runUntil([&] { return reliable() == count; }, 120s));
    network.impairment = {};
    ASSERT_TRUE(network.runUntil([&] { return network.a->sender()->idle(); }, 120s));

    std::set<std::string> unreliable_seen;
    std::uint32_t next = 0;
    for (const auto &message : network.endpoint_b.messages) {
        if (message.stream == 0) {
            EXPECT_EQ(text(message.payload), "r" + std::to_string(next++));
        }
        else {
            EXPECT_TRUE(unreliable_seen.insert(text(message.payload)).second);
        }
    }
    EXPECT_LT(unreliable_seen.size(), count);
    EXPECT_GT(unreliable_seen.size(), count / 2);
    EXPECT_GT(forward_tsns, 0);
    EXPECT_FALSE(network.endpoint_a.closed);
}

TEST_P(Transfer, SkipsAbandonedOrderedMessages)
{
    Network network{{}, {}, GetParam()};
    network.impairment = {.loss = 0.2};
    connect(network);
    constexpr std::uint32_t count = 200;
    for (std::uint32_t i = 0; i < count; ++i) {
        ASSERT_TRUE(network.a->send(3, 53, bufferOf(std::to_string(i)), {.max_retransmits = 0}, network.now));
        network.run(5ms);
    }
    network.impairment = {};
    ASSERT_TRUE(network.runUntil([&] { return network.a->sender()->idle(); }, 120s));
    network.run(1s);
    ASSERT_FALSE(network.endpoint_b.messages.empty());
    EXPECT_LT(network.endpoint_b.messages.size(), count);
    auto previous = -1;
    for (const auto &message : network.endpoint_b.messages) {
        const auto value = std::stoi(text(message.payload));
        EXPECT_GT(value, previous);
        previous = value;
    }
}

TEST_P(Transfer, RecoversAfterBurstLoss)
{
    Network network{{}, {}, GetParam()};
    connect(network);
    constexpr std::uint32_t count = 400;
    for (std::uint32_t i = 0; i < count; ++i) {
        ASSERT_TRUE(network.a->send(0, 53, patterned(1000, i), {}, network.now));
    }
    network.run(50ms);
    network.filter = [](std::span<const std::byte>, bool) {
        return false;
    };
    network.run(3s);
    network.filter = {};
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.messages.size() == count; }, 60s));
    for (std::uint32_t i = 0; i < count; ++i) {
        EXPECT_TRUE(intact(network.endpoint_b.messages[i].payload, 1000, i));
    }
    EXPECT_FALSE(network.endpoint_a.closed);
    ASSERT_TRUE(network.a->send(0, 53, bufferOf("after"), {}, network.now));
    EXPECT_TRUE(network.runUntil([&] { return network.endpoint_b.messages.size() == count + 1; }, 5s));
}

TEST_P(Transfer, CrossesTheTsnWrap)
{
    Association::Config a_config;
    std::vector<std::byte> prefix(StateCookie::Key{}.size());
    for (const auto value : {0x01, 0x02, 0x03, 0x04, 0xff, 0xff, 0xff, 0x80}) {
        prefix.push_back(static_cast<std::byte>(value));
    }
    a_config.random = scripted(std::move(prefix), GetParam());
    Network network{a_config, {}, GetParam()};
    network.impairment = {.loss = 0.05, .duplication = 0.05, .reordering = 0.2};
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil(
        [&] { return network.a->state() == State::Established && network.b->state() == State::Established; }, 10s));
    std::uint32_t first_tsn = 0;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        const auto parsed = Packet::parse(packet);
        if (to_b && first_tsn == 0 && parsed->chunks.front().type == static_cast<std::uint8_t>(ChunkType::Data)) {
            first_tsn = DataChunk::parse(parsed->chunks.front())->tsn;
        }
        return true;
    };
    for (std::uint32_t i = 0; i < 300; ++i) {
        ASSERT_TRUE(network.a->send(0, 53, patterned(500, i), {}, network.now));
    }
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.messages.size() == 300; }, 60s));
    for (std::uint32_t i = 0; i < 300; ++i) {
        EXPECT_TRUE(intact(network.endpoint_b.messages[i].payload, 500, i));
    }
    EXPECT_EQ(first_tsn, 0xffffff80U);
}

TEST_P(Transfer, CarriesTrafficBothWaysAtOnce)
{
    Network network{{}, {}, GetParam()};
    network.impairment = {.loss = 0.05, .reordering = 0.1};
    connect(network);
    for (std::uint32_t i = 0; i < 100; ++i) {
        ASSERT_TRUE(network.a->send(0, 53, patterned(2000, i), {}, network.now));
        ASSERT_TRUE(network.b->send(1, 53, patterned(2000, i + 1000), {}, network.now));
    }
    ASSERT_TRUE(network.runUntil(
        [&] { return network.endpoint_a.messages.size() == 100 && network.endpoint_b.messages.size() == 100; }, 60s));
    for (std::uint32_t i = 0; i < 100; ++i) {
        EXPECT_TRUE(intact(network.endpoint_b.messages[i].payload, 2000, i));
        EXPECT_TRUE(intact(network.endpoint_a.messages[i].payload, 2000, i + 1000));
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, Transfer, testing::Values(1U, 2U, 3U, 4U, 5U));

TEST(Throughput, OpensTheWindowOnACleanPath)
{
    Network network;
    connect(network);
    const auto start = network.now;
    for (std::uint32_t i = 0; i < 16; ++i) {
        ASSERT_TRUE(network.a->send(0, 53, patterned(262144, i), {}, network.now));
    }
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.messages.size() == 16; }, 60s));
    EXPECT_LT(network.now - start, 3s);
    EXPECT_GT(network.a->sender()->cwnd(), 100U * 1191);
}

}  // namespace
}  // namespace endgate::rtc::sctp
