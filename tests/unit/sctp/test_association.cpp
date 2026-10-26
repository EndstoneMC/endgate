#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "bytes.h"
#include "endgate/network/rtc/sctp/crc32c.h"
#include "network.h"

namespace endgate::rtc::sctp {
namespace {

using namespace std::chrono_literals;
using State = Association::State;
using CloseReason = Association::CloseReason;

bool established(Network &network)
{
    return network.a->state() == State::Established && network.b->state() == State::Established;
}

std::vector<std::byte> sealed(std::vector<std::byte> packet)
{
    std::fill_n(packet.begin() + 8, 4, std::byte{0});
    const auto checksum = crc32c(packet);
    for (std::size_t i = 0; i < 4; ++i) {
        packet[8 + i] = static_cast<std::byte>(checksum >> (8 * i));
    }
    return packet;
}

std::vector<std::byte> withChunk(std::uint32_t tag, std::vector<std::byte> chunk)
{
    auto packet = bytes({0x13, 0x88, 0x13, 0x88});
    for (auto shift = 24; shift >= 0; shift -= 8) {
        packet.push_back(static_cast<std::byte>(tag >> shift));
    }
    packet.resize(12);
    packet.insert(packet.end(), chunk.begin(), chunk.end());
    return sealed(std::move(packet));
}

std::uint32_t tagOf(std::span<const std::byte> packet)
{
    return Packet::parse(packet)->header.verification_tag;
}

TEST(Association, OpensActivelyAgainstPassivePeer)
{
    Network network;
    network.a->connect(network.now);
    EXPECT_EQ(network.a->state(), State::CookieWait);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    EXPECT_EQ(network.endpoint_a.established, 1U);
    EXPECT_EQ(network.endpoint_b.established, 1U);
    EXPECT_EQ(network.a->inboundStreams(), 1024);
    EXPECT_EQ(network.a->outboundStreams(), 1024);
    EXPECT_TRUE(network.a->supportsPartialReliability());
}

TEST(Association, StaysStatelessUntilCookieEcho)
{
    Network network;
    network.filter = [](std::span<const std::byte> packet, bool to_b) {
        return to_b || firstChunk(packet) != static_cast<std::uint8_t>(ChunkType::InitAck);
    };
    network.a->connect(network.now);
    network.run(100ms);
    EXPECT_EQ(network.b->state(), State::Closed);
    EXPECT_EQ(network.a->state(), State::CookieWait);
    network.filter = {};
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 3s));
}

TEST(Association, ResolvesSimultaneousInit)
{
    Network network;
    network.a->connect(network.now);
    network.b->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    network.run(1s);
    EXPECT_EQ(network.endpoint_a.established, 1U);
    EXPECT_EQ(network.endpoint_b.established, 1U);
    EXPECT_FALSE(network.endpoint_a.closed);
    EXPECT_FALSE(network.endpoint_b.closed);

    ASSERT_TRUE(network.a->send(1, 51, bufferOf("ping"), {}, network.now));
    ASSERT_TRUE(network.b->send(2, 51, bufferOf("pong"), {}, network.now));
    ASSERT_TRUE(network.runUntil(
        [&] { return !network.endpoint_a.messages.empty() && !network.endpoint_b.messages.empty(); }, 1s));
    EXPECT_EQ(text(network.endpoint_b.messages[0].payload), "ping");
    EXPECT_EQ(text(network.endpoint_a.messages[0].payload), "pong");
}

TEST(Association, ResolvesInitCollisionWhateverIsLost)
{
    for (const auto lost : {ChunkType::Init, ChunkType::InitAck, ChunkType::CookieEcho, ChunkType::CookieAck}) {
        for (const auto direction : {true, false}) {
            Network network;
            auto dropped = false;
            network.filter = [&](std::span<const std::byte> packet, bool to_b) {
                if (!dropped && to_b == direction && firstChunk(packet) == static_cast<std::uint8_t>(lost)) {
                    dropped = true;
                    return false;
                }
                return true;
            };
            network.a->connect(network.now);
            network.b->connect(network.now);
            ASSERT_TRUE(network.runUntil([&] { return established(network); }, 5s))
                << static_cast<int>(lost) << " " << direction;
            network.run(5s);
            EXPECT_EQ(network.endpoint_a.established, 1U) << static_cast<int>(lost) << " " << direction;
            EXPECT_EQ(network.endpoint_b.established, 1U) << static_cast<int>(lost) << " " << direction;
            EXPECT_FALSE(network.endpoint_a.closed);
            EXPECT_FALSE(network.endpoint_b.closed);

            ASSERT_TRUE(network.a->send(0, 53, bufferOf("x"), {}, network.now));
            ASSERT_TRUE(network.b->send(0, 53, bufferOf("y"), {}, network.now));
            EXPECT_TRUE(network.runUntil(
                [&] { return network.endpoint_a.messages.size() == 1 && network.endpoint_b.messages.size() == 1; },
                5s));
        }
    }
}

TEST(Association, ResolvesInitCollisionWithStaggeredStarts)
{
    for (const auto offset : {0ms, 5ms, 19ms, 20ms, 21ms, 39ms, 40ms, 41ms, 60ms}) {
        Network network;
        network.a->connect(network.now);
        network.run(offset);
        network.b->connect(network.now);
        ASSERT_TRUE(network.runUntil([&] { return established(network); }, 2s)) << offset.count();
        network.run(3s);
        EXPECT_EQ(network.endpoint_a.established, 1U) << offset.count();
        EXPECT_EQ(network.endpoint_b.established, 1U) << offset.count();
        ASSERT_TRUE(network.a->send(0, 53, bufferOf("x"), {}, network.now));
        ASSERT_TRUE(network.b->send(0, 53, bufferOf("y"), {}, network.now));
        EXPECT_TRUE(network.runUntil(
            [&] { return network.endpoint_a.messages.size() == 1 && network.endpoint_b.messages.size() == 1; }, 2s))
            << offset.count();
    }
}

TEST(Association, AnswersCollidingInitWithItsOwnTag)
{
    Network network;
    std::vector<std::uint32_t> a_init_tags;
    std::vector<std::uint32_t> a_init_ack_tags;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        const auto parsed = Packet::parse(packet);
        if (to_b && parsed->chunks.front().type == static_cast<std::uint8_t>(ChunkType::Init)) {
            a_init_tags.push_back(InitChunk::parse(parsed->chunks.front())->initiate_tag);
        }
        if (to_b && parsed->chunks.front().type == static_cast<std::uint8_t>(ChunkType::InitAck)) {
            a_init_ack_tags.push_back(InitChunk::parse(parsed->chunks.front())->initiate_tag);
        }
        return true;
    };
    network.a->connect(network.now);
    network.b->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    ASSERT_EQ(a_init_tags.size(), 1U);
    ASSERT_EQ(a_init_ack_tags.size(), 1U);
    EXPECT_EQ(a_init_ack_tags[0], a_init_tags[0]);
}

TEST(Association, AnswersCapturedUsrsctpInit)
{
    const auto init = bytes({
        0x13, 0x88, 0x13, 0x88, 0x00, 0x00, 0x00, 0x00, 0xaa, 0x01, 0x9d, 0x33, 0x01, 0x00, 0x00, 0x5a, 0x0e, 0xdd,
        0xca, 0x08, 0x00, 0x02, 0x00, 0x00, 0x03, 0xe8, 0x03, 0xe8, 0x55, 0x08, 0x36, 0x37, 0x80, 0x00, 0x00, 0x04,
        0xc0, 0x00, 0x00, 0x04, 0x80, 0x08, 0x00, 0x09, 0xc0, 0x0f, 0xc1, 0x80, 0x82, 0x00, 0x00, 0x00, 0x80, 0x02,
        0x00, 0x24, 0xc5, 0xa8, 0x61, 0x55, 0x09, 0x0e, 0x6f, 0x42, 0x00, 0x50, 0x63, 0x4c, 0xc8, 0xd6, 0xb9, 0x08,
        0xdf, 0xd5, 0x3e, 0x17, 0xc9, 0x9c, 0xb1, 0x43, 0x28, 0x4e, 0xaf, 0x64, 0x68, 0x2a, 0xc2, 0x97, 0x80, 0x04,
        0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x80, 0x03, 0x00, 0x06, 0x80, 0xc1, 0x00, 0x00,
    });
    Network network;
    network.b->handlePacket(init, network.now);
    EXPECT_EQ(network.b->state(), State::Closed);
    ASSERT_EQ(network.endpoint_b.outbox.size(), 1U);
    const auto packet = Packet::parse(network.endpoint_b.outbox[0]);
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->header.verification_tag, 0x0eddca08U);
    ASSERT_EQ(packet->chunks.size(), 1U);
    const auto init_ack = InitChunk::parse(packet->chunks[0]);
    ASSERT_TRUE(init_ack);
    EXPECT_NE(init_ack->initiate_tag, 0U);
    EXPECT_EQ(init_ack->outbound_streams, 1024);
    EXPECT_EQ(init_ack->inbound_streams, 1024);
    ASSERT_EQ(init_ack->parameters.size(), 3U);
    EXPECT_EQ(init_ack->parameters[0].type, static_cast<std::uint16_t>(ParameterType::StateCookie));
    EXPECT_EQ(init_ack->parameters[1].type, static_cast<std::uint16_t>(ParameterType::ForwardTsnSupported));
    EXPECT_EQ(init_ack->parameters[2].type, static_cast<std::uint16_t>(ParameterType::SupportedExtensions));
    EXPECT_EQ(std::vector<std::byte>(init_ack->parameters[2].value.begin(), init_ack->parameters[2].value.end()),
              bytes({0x82, 0xc0}));
}

TEST(Association, ReportsUnrecognizedInitParameters)
{
    Network network;
    auto chunk =
        bytes({0x01, 0x00, 0x00, 0x20, 0x11, 0x22, 0x33, 0x44, 0x00, 0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10,
               0x00, 0x00, 0x00, 0x01, 0xc0, 0x00, 0x00, 0x04, 0xc0, 0x06, 0x00, 0x08, 0x01, 0x02, 0x03, 0x04});
    network.b->handlePacket(withChunk(0, chunk), network.now);
    ASSERT_EQ(network.endpoint_b.outbox.size(), 1U);
    const auto init_ack = InitChunk::parse(Packet::parse(network.endpoint_b.outbox[0])->chunks[0]);
    ASSERT_TRUE(init_ack);
    ASSERT_EQ(init_ack->parameters.size(), 4U);
    EXPECT_EQ(init_ack->parameters[3].type, static_cast<std::uint16_t>(ParameterType::UnrecognizedParameter));
    EXPECT_EQ(std::vector<std::byte>(init_ack->parameters[3].value.begin(), init_ack->parameters[3].value.end()),
              bytes({0xc0, 0x06, 0x00, 0x08, 0x01, 0x02, 0x03, 0x04}));
}

TEST(Association, RefusesInvalidInit)
{
    Network network;
    network.b->handlePacket(withChunk(0, bytes({0x01, 0x00, 0x00, 0x14, 0x11, 0x22, 0x33, 0x44, 0x00, 0x01,
                                                0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01})),
                            network.now);
    ASSERT_EQ(network.endpoint_b.outbox.size(), 1U);
    EXPECT_EQ(firstChunk(network.endpoint_b.outbox[0]), static_cast<std::uint8_t>(ChunkType::Abort));
    EXPECT_EQ(tagOf(network.endpoint_b.outbox[0]), 0x11223344U);
    network.endpoint_b.outbox.clear();
    network.b->handlePacket(withChunk(0, bytes({0x01, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
                                                0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01})),
                            network.now);
    EXPECT_TRUE(network.endpoint_b.outbox.empty());
    network.b->handlePacket(withChunk(0x11223344, bytes({0x01, 0x00, 0x00, 0x14, 0x11, 0x22, 0x33, 0x44, 0x00, 0x01,
                                                         0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01})),
                            network.now);
    EXPECT_TRUE(network.endpoint_b.outbox.empty());
    EXPECT_EQ(network.b->state(), State::Closed);
}

TEST(Association, RetransmitsInitThenGivesUp)
{
    Association::Config config;
    config.max_init_retransmits = 3;
    Network network{config};
    network.filter = [](std::span<const std::byte>, bool to_b) {
        return !to_b;
    };
    network.a->connect(network.now);
    network.run(30s);
    EXPECT_EQ(network.sent, 4U);
    EXPECT_EQ(network.endpoint_a.closed, CloseReason::Timeout);
    EXPECT_EQ(network.a->state(), State::Closed);
}

TEST(Association, RetransmitsCookieEcho)
{
    Network network;
    auto dropped = 0;
    network.filter = [&](std::span<const std::byte> packet, bool) {
        return firstChunk(packet) != static_cast<std::uint8_t>(ChunkType::CookieEcho) || ++dropped > 2;
    };
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 10s));
    EXPECT_EQ(dropped, 3);
}

TEST(Association, IgnoresTamperedCookie)
{
    Network network;
    std::vector<std::byte> cookie_echo;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (to_b && firstChunk(packet) == static_cast<std::uint8_t>(ChunkType::CookieEcho)) {
            cookie_echo.assign(packet.begin(), packet.end());
            return false;
        }
        return true;
    };
    network.a->connect(network.now);
    network.run(100ms);
    ASSERT_EQ(cookie_echo.size(), 16U + 73 + 3);
    for (std::size_t i = 16; i < 16 + 73; ++i) {
        auto altered = cookie_echo;
        altered[i] ^= std::byte{0x01};
        network.b->handlePacket(sealed(altered), network.now);
        EXPECT_EQ(network.b->state(), State::Closed) << i;
    }
    EXPECT_EQ(network.b->droppedPackets(), 73U);
    network.b->handlePacket(cookie_echo, network.now);
    EXPECT_EQ(network.b->state(), State::Established);
}

TEST(Association, AnswersStaleCookieWithError)
{
    Association::Config config;
    config.cookie_lifetime = 100ms;
    Network network{{}, config};
    network.latency = 200ms;
    network.a->connect(network.now);
    network.run(2s);
    EXPECT_EQ(network.b->state(), State::Closed);
    EXPECT_EQ(network.endpoint_a.closed, CloseReason::Timeout);
}

TEST(Association, ClosesOnPeerAbort)
{
    Network network;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    network.a->abort(network.now);
    EXPECT_EQ(network.endpoint_a.closed, CloseReason::Aborted);
    network.run(100ms);
    EXPECT_EQ(network.endpoint_b.closed, CloseReason::PeerAborted);
    EXPECT_EQ(network.b->state(), State::Closed);
}

TEST(Association, ShutsDownGracefullyAfterDeliveringEverything)
{
    Network network;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    for (auto i = 0; i < 50; ++i) {
        ASSERT_TRUE(network.a->send(0, 53, ByteBuf::copyBuffer(std::vector<std::byte>(1000)), {}, network.now));
    }
    network.a->shutdown(network.now);
    EXPECT_FALSE(network.a->send(0, 53, bufferOf("late"), {}, network.now));
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_a.closed && network.endpoint_b.closed; }, 10s));
    EXPECT_EQ(network.endpoint_a.closed, CloseReason::Shutdown);
    EXPECT_EQ(network.endpoint_b.closed, CloseReason::Shutdown);
    EXPECT_EQ(network.endpoint_b.messages.size(), 50U);
}

TEST(Association, ShutsDownWhenBothSidesAskAtOnce)
{
    Network network;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    network.a->shutdown(network.now);
    network.b->shutdown(network.now);
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_a.closed && network.endpoint_b.closed; }, 10s));
    EXPECT_EQ(network.endpoint_a.closed, CloseReason::Shutdown);
    EXPECT_EQ(network.endpoint_b.closed, CloseReason::Shutdown);
}

TEST(Association, RetransmitsShutdownUntilAcknowledged)
{
    Network network;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    auto dropped = 0;
    network.filter = [&](std::span<const std::byte> packet, bool) {
        return !hasChunk(packet, ChunkType::ShutdownAck) || ++dropped > 2;
    };
    network.a->shutdown(network.now);
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_a.closed && network.endpoint_b.closed; }, 20s));
    EXPECT_EQ(network.endpoint_a.closed, CloseReason::Shutdown);
}

TEST(Association, DropsPacketsWithWrongTagOrPorts)
{
    Network network;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    const auto dropped = network.b->droppedPackets();
    network.b->handlePacket(withChunk(0x12345678, bytes({0x0b, 0x00, 0x00, 0x04})), network.now);
    auto wrong_port = withChunk(0, bytes({0x0b, 0x00, 0x00, 0x04}));
    wrong_port[1] = std::byte{0x89};
    network.b->handlePacket(sealed(wrong_port), network.now);
    network.b->handlePacket(bytes({1, 2, 3}), network.now);
    EXPECT_EQ(network.b->droppedPackets(), dropped + 3);
    EXPECT_EQ(network.b->state(), State::Established);
}

TEST(Association, ReportsUnrecognizedChunksByTheirHighBits)
{
    Network network;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    network.run(1s);
    std::uint32_t tag = 0;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (to_b) {
            tag = tagOf(packet);
        }
        return true;
    };
    ASSERT_TRUE(network.a->send(0, 51, bufferOf("x"), {}, network.now));
    network.run(1s);
    ASSERT_NE(tag, 0U);

    const auto errors = [&](std::vector<std::byte> chunks) {
        network.endpoint_b.outbox.clear();
        network.b->handlePacket(withChunk(tag, std::move(chunks)), network.now);
        auto count = 0;
        for (const auto &packet : network.endpoint_b.outbox) {
            count += hasChunk(packet, ChunkType::Error) ? 1 : 0;
        }
        network.endpoint_b.outbox.clear();
        return count;
    };
    EXPECT_EQ(errors(bytes({0x09, 0x00, 0x00, 0x04})), 0);
    EXPECT_EQ(errors(bytes({0x49, 0x00, 0x00, 0x04})), 1);
    EXPECT_EQ(errors(bytes({0x89, 0x00, 0x00, 0x04})), 0);
    EXPECT_EQ(errors(bytes({0xc9, 0x00, 0x00, 0x04})), 1);
    EXPECT_EQ(errors(bytes({0x40, 0x00, 0x00, 0x04})), 1);
    EXPECT_EQ(network.b->state(), State::Established);
}

TEST(Association, ReportsDataOnStreamBeyondNegotiatedCount)
{
    Association::Config small;
    small.streams = 4;
    Network network{{}, small};
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    EXPECT_EQ(network.a->outboundStreams(), 4);
    EXPECT_FALSE(network.a->send(4, 51, bufferOf("x"), {}, network.now));
    EXPECT_TRUE(network.a->send(3, 51, bufferOf("x"), {}, network.now));
    network.run(1s);
    EXPECT_EQ(network.endpoint_b.messages.size(), 1U);
}

TEST(Association, AnswersHeartbeatsAndMeasuresRtt)
{
    Association::Config config;
    config.heartbeat_interval = 1s;
    Network network{config};
    network.latency = 50ms;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    auto heartbeats = 0;
    auto acks = 0;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        heartbeats += to_b && hasChunk(packet, ChunkType::Heartbeat) ? 1 : 0;
        acks += !to_b && hasChunk(packet, ChunkType::HeartbeatAck) ? 1 : 0;
        return true;
    };
    network.run(10s);
    EXPECT_GE(heartbeats, 5);
    EXPECT_GE(acks, heartbeats - 1);
    EXPECT_EQ(network.a->sender()->srtt(), 100ms);
    EXPECT_FALSE(network.endpoint_a.closed);
}

TEST(Association, IgnoresHeartbeatTooLargeToEcho)
{
    Network network;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    std::uint32_t tag = 0;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (to_b) {
            tag = tagOf(packet);
        }
        return true;
    };
    ASSERT_TRUE(network.a->send(0, 51, bufferOf("x"), {}, network.now));
    network.run(1s);
    const auto heartbeat = [&](std::size_t size) {
        std::vector<std::byte> value;
        ByteWriter(value).writeTlv(static_cast<std::uint16_t>(ParameterType::HeartbeatInfo),
                                   std::vector<std::byte>(size));
        std::vector<std::byte> buffer;
        PacketBuilder builder(buffer, {.source_port = 5000, .destination_port = 5000, .verification_tag = tag}, 65535);
        builder.addChunk(ChunkType::Heartbeat, 0, value);
        network.endpoint_b.outbox.clear();
        network.b->handlePacket(builder.build(), network.now);
        auto acks = 0;
        for (const auto &packet : network.endpoint_b.outbox) {
            acks += hasChunk(packet, ChunkType::HeartbeatAck) ? 1 : 0;
        }
        return acks;
    };
    EXPECT_EQ(heartbeat(16), 1);
    EXPECT_EQ(heartbeat(1191 - 20), 1);
    EXPECT_EQ(heartbeat(1191 - 19), 0);
    EXPECT_EQ(heartbeat(16000), 0);
}

TEST(Association, AbortsWhenHeartbeatsGoUnanswered)
{
    Association::Config config;
    config.heartbeat_interval = 1s;
    config.max_retransmissions = 3;
    Network network{config};
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    network.filter = [](std::span<const std::byte>, bool to_b) {
        return to_b;
    };
    network.run(60s);
    EXPECT_EQ(network.endpoint_a.closed, CloseReason::Timeout);
    EXPECT_EQ(network.endpoint_b.closed, CloseReason::PeerAborted);
}

TEST(Association, AbortsAfterTooManyRetransmissions)
{
    Association::Config config;
    config.max_retransmissions = 2;
    config.heartbeat_interval = {};
    Network network{config};
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    network.filter = [](std::span<const std::byte>, bool) {
        return false;
    };
    ASSERT_TRUE(network.a->send(0, 51, bufferOf("x"), {}, network.now));
    network.run(60s);
    EXPECT_EQ(network.endpoint_a.closed, CloseReason::Timeout);
}

TEST(Association, RecognisesRestartedPeer)
{
    Network network;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));

    Endpoint restarted;
    Association::Config config;
    config.random = seeded(99);
    Association a2{restarted, config};
    a2.connect(network.now);
    for (auto round = 0; round < 4; ++round) {
        for (auto &packet : restarted.outbox) {
            network.b->handlePacket(packet, network.now);
        }
        restarted.outbox.clear();
        for (auto &packet : network.endpoint_b.outbox) {
            a2.handlePacket(packet, network.now);
        }
        network.endpoint_b.outbox.clear();
    }
    EXPECT_EQ(a2.state(), State::Established);
    EXPECT_EQ(network.endpoint_b.restarted, 1U);
    EXPECT_EQ(network.b->state(), State::Established);

    ASSERT_TRUE(a2.send(0, 51, bufferOf("again"), {}, network.now));
    for (auto &packet : restarted.outbox) {
        network.b->handlePacket(packet, network.now);
    }
    ASSERT_EQ(network.endpoint_b.messages.size(), 1U);
    EXPECT_EQ(text(network.endpoint_b.messages[0].payload), "again");
}

TEST(Association, HonoursOnlyTheLastSackInAPacket)
{
    Network network;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    network.run(1s);
    std::vector<std::uint32_t> tsns;
    std::uint32_t tag = 0;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        const auto parsed = Packet::parse(packet);
        if (!to_b) {
            tag = parsed->header.verification_tag;
            return false;
        }
        for (const auto &chunk : parsed->chunks) {
            if (const auto data = chunk.type == 0 ? DataChunk::parse(chunk) : std::nullopt) {
                tsns.push_back(data->tsn);
            }
        }
        return true;
    };
    ASSERT_TRUE(network.a->send(0, 51, bufferOf("one"), {}, network.now));
    ASSERT_TRUE(network.a->send(0, 51, bufferOf("two"), {}, network.now));
    network.run(100ms);
    ASSERT_EQ(tsns.size(), 2U);
    ASSERT_NE(tag, 0U);

    std::vector<std::byte> buffer;
    PacketBuilder builder(buffer, {.source_port = 5000, .destination_port = 5000, .verification_tag = tag}, 1200);
    builder.addSack({.cumulative_tsn_ack = tsns[1], .a_rwnd = 100000, .gap_blocks = {}, .duplicate_tsns = {}});
    builder.addSack({.cumulative_tsn_ack = tsns[0], .a_rwnd = 100000, .gap_blocks = {}, .duplicate_tsns = {}});
    network.a->handlePacket(builder.build(), network.now);
    EXPECT_EQ(network.a->sender()->outstandingChunks(), 1U);
}

TEST(Association, ForgetsItsShutdownWhenThePeerRestarts)
{
    Network network;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    network.filter = [](std::span<const std::byte>, bool to_b) {
        return to_b;
    };
    network.b->shutdown(network.now);
    network.run(10ms);
    ASSERT_EQ(network.b->state(), State::ShutdownSent);

    Endpoint restarted;
    Association::Config config;
    config.random = seeded(99);
    Association a2{restarted, config};
    a2.connect(network.now);
    auto shutdowns = 0;
    for (auto step = 0; step < 600; ++step) {
        for (auto &packet : std::exchange(restarted.outbox, {})) {
            network.b->handlePacket(packet, network.now);
        }
        for (auto &packet : std::exchange(network.endpoint_b.outbox, {})) {
            shutdowns += hasChunk(packet, ChunkType::Shutdown) ? 1 : 0;
            a2.handlePacket(packet, network.now);
        }
        network.now += 100ms;
        network.b->handleTimeout(network.now);
        a2.handleTimeout(network.now);
    }
    EXPECT_EQ(network.endpoint_b.restarted, 1U);
    EXPECT_EQ(network.b->state(), State::Established);
    EXPECT_FALSE(network.endpoint_b.closed);
    EXPECT_EQ(shutdowns, 0);
}

TEST(Association, ListenerMayCallBackIntoTheAssociation)
{
    Network network;
    network.a->connect(network.now);
    ASSERT_TRUE(network.runUntil([&] { return established(network); }, 1s));
    network.endpoint_b.on_message = [&](const Message &message) {
        if (text(message.payload) == "ping") {
            EXPECT_TRUE(network.b->send(message.stream, 51, bufferOf("pong"), {}, network.now));
        }
    };
    ASSERT_TRUE(network.a->send(7, 51, bufferOf("ping"), {}, network.now));
    ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_a.messages.empty(); }, 1s));
    EXPECT_EQ(text(network.endpoint_a.messages[0].payload), "pong");
    EXPECT_EQ(network.endpoint_a.messages[0].stream, 7);
}

}  // namespace
}  // namespace endgate::rtc::sctp
