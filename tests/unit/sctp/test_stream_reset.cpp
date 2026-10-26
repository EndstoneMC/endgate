#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/network/rtc/sctp/crc32c.h"
#include "network.h"

namespace endgate::rtc::sctp {
namespace {

using namespace std::chrono_literals;
using State = Association::State;
using Streams = std::vector<std::uint16_t>;

void connect(Network &network)
{
    network.a->connect(network.now);
    network.b->connect(network.now);
    ASSERT_TRUE(network.runUntil(
        [&] { return network.a->state() == State::Established && network.b->state() == State::Established; }, 5s));
}

std::vector<ReconfigResponse> responsesIn(std::span<const std::byte> packet)
{
    std::vector<ReconfigResponse> responses;
    const auto parsed = Packet::parse(packet);
    for (const auto &chunk : parsed->chunks) {
        if (chunk.type != static_cast<std::uint8_t>(ChunkType::ReConfig)) {
            continue;
        }
        for (const auto &parameter : *Tlv::parseAll(chunk.value)) {
            if (const auto response = ReconfigResponse::parse(parameter)) {
                responses.push_back(*response);
            }
        }
    }
    return responses;
}

TEST(StreamReset, ResetsAfterQueuedMessagesAreDelivered)
{
    Network network;
    connect(network);
    for (auto i = 0; i < 5; ++i) {
        ASSERT_TRUE(network.a->send(1, 51, bufferOf("m" + std::to_string(i)), {}, network.now));
    }
    const Streams streams{1};
    ASSERT_TRUE(network.a->resetStreams(streams, network.now));
    EXPECT_FALSE(network.a->send(1, 51, bufferOf("refused"), {}, network.now));
    ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_a.outgoing_resets.empty(); }, 5s));

    EXPECT_EQ(network.endpoint_a.outgoing_resets[0], std::pair(streams, true));
    ASSERT_EQ(network.endpoint_b.incoming_resets.size(), 1U);
    EXPECT_EQ(network.endpoint_b.incoming_resets[0], streams);
    ASSERT_EQ(network.endpoint_b.messages.size(), 5U);
    EXPECT_EQ(text(network.endpoint_b.messages[4].payload), "m4");

    ASSERT_TRUE(network.a->send(1, 51, bufferOf("reopened"), {}, network.now));
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.messages.size() == 6; }, 5s));
    EXPECT_EQ(text(network.endpoint_b.messages[5].payload), "reopened");
}

TEST(StreamReset, DefersResetUntilLostDataArrives)
{
    Network network;
    connect(network);
    auto dropped = false;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (to_b && !dropped && hasChunk(packet, ChunkType::Data)) {
            dropped = true;
            return false;
        }
        return true;
    };
    ASSERT_TRUE(network.a->send(1, 51, bufferOf("lost once"), {}, network.now));
    ASSERT_TRUE(network.a->send(2, 51, bufferOf("other stream"), {}, network.now));
    const Streams streams{1};
    ASSERT_TRUE(network.a->resetStreams(streams, network.now));

    std::vector<ReconfigResult> results;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (!to_b) {
            for (const auto &response : responsesIn(packet)) {
                results.push_back(response.result);
            }
        }
        if (to_b && !dropped && hasChunk(packet, ChunkType::Data)) {
            dropped = true;
            return false;
        }
        return true;
    };
    ASSERT_TRUE(network.a->send(2, 51, bufferOf("more"), {}, network.now));
    ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_a.outgoing_resets.empty(); }, 10s));
    EXPECT_TRUE(network.endpoint_a.outgoing_resets[0].second);
    ASSERT_EQ(network.endpoint_b.incoming_resets.size(), 1U);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results.front(), ReconfigResult::InProgress);
    EXPECT_EQ(results.back(), ReconfigResult::SuccessPerformed);

    std::vector<std::string> stream_one;
    for (const auto &message : network.endpoint_b.messages) {
        if (message.stream == 1) {
            stream_one.push_back(text(message.payload));
        }
    }
    EXPECT_EQ(stream_one, std::vector<std::string>{"lost once"});
}

TEST(StreamReset, HoldsNewDataBehindADeferredReset)
{
    Network network;
    connect(network);
    std::vector<std::vector<std::byte>> held;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (to_b && hasChunk(packet, ChunkType::Data) && held.empty()) {
            held.emplace_back(packet.begin(), packet.end());
            return false;
        }
        return true;
    };
    ASSERT_TRUE(network.a->send(1, 51, bufferOf("before"), {}, network.now));
    const Streams streams{1};
    ASSERT_TRUE(network.a->resetStreams(streams, network.now));
    network.run(60ms);
    EXPECT_TRUE(network.endpoint_a.outgoing_resets.empty());
    EXPECT_TRUE(network.endpoint_b.incoming_resets.empty());

    network.filter = {};
    ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_a.outgoing_resets.empty(); }, 10s));
    ASSERT_TRUE(network.a->send(1, 51, bufferOf("after"), {}, network.now));
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.messages.size() == 2; }, 5s));
    EXPECT_EQ(text(network.endpoint_b.messages[0].payload), "before");
    EXPECT_EQ(text(network.endpoint_b.messages[1].payload), "after");
}

TEST(StreamReset, ResetsSeveralStreamsInOneRequest)
{
    Network network;
    connect(network);
    ASSERT_TRUE(network.a->send(3, 51, bufferOf("x"), {}, network.now));
    ASSERT_TRUE(network.a->send(5, 51, bufferOf("y"), {}, network.now));
    network.run(200ms);
    const Streams streams{3, 5};
    ASSERT_TRUE(network.a->resetStreams(streams, network.now));
    ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_a.outgoing_resets.empty(); }, 5s));
    EXPECT_EQ(network.endpoint_a.outgoing_resets[0].first, streams);
    EXPECT_EQ(network.endpoint_b.incoming_resets[0], streams);
}

TEST(StreamReset, ClosesBothDirectionsLikeADataChannel)
{
    Network network;
    connect(network);
    ASSERT_TRUE(network.a->send(1, 51, bufferOf("a"), {}, network.now));
    ASSERT_TRUE(network.b->send(1, 51, bufferOf("b"), {}, network.now));
    network.run(200ms);
    const Streams streams{1};
    ASSERT_TRUE(network.a->resetStreams(streams, network.now));
    ASSERT_TRUE(network.b->resetStreams(streams, network.now));
    ASSERT_TRUE(network.runUntil(
        [&] {
            return !network.endpoint_a.outgoing_resets.empty() && !network.endpoint_b.outgoing_resets.empty() &&
                   !network.endpoint_a.incoming_resets.empty() && !network.endpoint_b.incoming_resets.empty();
        },
        5s));
    EXPECT_TRUE(network.endpoint_a.outgoing_resets[0].second);
    EXPECT_TRUE(network.endpoint_b.outgoing_resets[0].second);
}

TEST(StreamReset, RetransmitsRequestAndResponse)
{
    for (const auto drop_request : {true, false}) {
        Network network;
        connect(network);
        auto dropped = false;
        network.filter = [&](std::span<const std::byte> packet, bool to_b) {
            if (!dropped && to_b == drop_request && hasChunk(packet, ChunkType::ReConfig)) {
                dropped = true;
                return false;
            }
            return true;
        };
        const Streams streams{2};
        ASSERT_TRUE(network.a->resetStreams(streams, network.now));
        ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_a.outgoing_resets.empty(); }, 10s));
        EXPECT_TRUE(dropped);
        EXPECT_TRUE(network.endpoint_a.outgoing_resets[0].second);
        EXPECT_EQ(network.endpoint_b.incoming_resets.size(), 1U);
    }
}

TEST(StreamReset, AnswersRepeatedAndOutOfSequenceRequests)
{
    Network network;
    connect(network);
    std::vector<std::byte> request;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (to_b && hasChunk(packet, ChunkType::ReConfig)) {
            request.assign(packet.begin(), packet.end());
        }
        return true;
    };
    const Streams streams{4};
    ASSERT_TRUE(network.a->resetStreams(streams, network.now));
    ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_a.outgoing_resets.empty(); }, 5s));
    ASSERT_FALSE(request.empty());

    network.endpoint_b.outbox.clear();
    network.b->handlePacket(request, network.now);
    ASSERT_FALSE(network.endpoint_b.outbox.empty());
    auto responses = responsesIn(network.endpoint_b.outbox.back());
    ASSERT_EQ(responses.size(), 1U);
    EXPECT_EQ(responses[0].result, ReconfigResult::SuccessPerformed);
    EXPECT_EQ(network.endpoint_b.incoming_resets.size(), 1U);

    auto parsed = Packet::parse(request);
    std::vector<std::byte> forged(request.begin(), request.end());
    const auto offset = static_cast<std::size_t>(parsed->chunks[0].value.data() - request.data()) + 4;
    forged[offset + 3] = static_cast<std::byte>(std::to_integer<std::uint8_t>(forged[offset + 3]) + 5);
    std::fill_n(forged.begin() + 8, 4, std::byte{0});
    const auto checksum = crc32c(forged);
    for (std::size_t i = 0; i < 4; ++i) {
        forged[8 + i] = static_cast<std::byte>(checksum >> (8 * i));
    }
    network.endpoint_b.outbox.clear();
    network.b->handlePacket(forged, network.now);
    ASSERT_FALSE(network.endpoint_b.outbox.empty());
    responses = responsesIn(network.endpoint_b.outbox.back());
    ASSERT_EQ(responses.size(), 1U);
    EXPECT_EQ(responses[0].result, ReconfigResult::ErrorBadSequenceNumber);
    EXPECT_EQ(network.endpoint_b.incoming_resets.size(), 1U);
}

TEST(StreamReset, FollowsARequestRetriedUnderANewSequenceNumber)
{
    Network network;
    connect(network);
    std::vector<std::byte> request;
    auto data_dropped = false;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (to_b && request.empty() && hasChunk(packet, ChunkType::ReConfig)) {
            request.assign(packet.begin(), packet.end());
            return false;
        }
        if (to_b && !data_dropped && hasChunk(packet, ChunkType::Data)) {
            data_dropped = true;
            return false;
        }
        return true;
    };
    ASSERT_TRUE(network.a->send(1, 51, bufferOf("lost once"), {}, network.now));
    const Streams streams{1};
    ASSERT_TRUE(network.a->resetStreams(streams, network.now));
    network.collect();
    ASSERT_FALSE(request.empty());

    const auto answer = [&](std::span<const std::byte> packet) {
        network.endpoint_b.outbox.clear();
        network.b->handlePacket(packet, network.now);
        for (const auto &sent : network.endpoint_b.outbox) {
            const auto responses = responsesIn(sent);
            if (!responses.empty()) {
                network.endpoint_b.outbox.clear();
                return std::optional{responses[0].result};
            }
        }
        network.endpoint_b.outbox.clear();
        return std::optional<ReconfigResult>{};
    };
    EXPECT_EQ(answer(request), ReconfigResult::InProgress);

    const auto parsed = Packet::parse(request);
    auto renumbered = request;
    const auto offset = static_cast<std::size_t>(parsed->chunks[0].value.data() - request.data()) + 4;
    renumbered[offset + 3] = static_cast<std::byte>(std::to_integer<std::uint8_t>(renumbered[offset + 3]) + 1);
    std::fill_n(renumbered.begin() + 8, 4, std::byte{0});
    const auto checksum = crc32c(renumbered);
    for (std::size_t i = 0; i < 4; ++i) {
        renumbered[8 + i] = static_cast<std::byte>(checksum >> (8 * i));
    }
    EXPECT_EQ(answer(renumbered), ReconfigResult::InProgress);

    ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_b.incoming_resets.empty(); }, 10s));
    EXPECT_EQ(network.endpoint_b.messages.size(), 1U);
    EXPECT_EQ(answer(renumbered), ReconfigResult::SuccessPerformed);
}

TEST(StreamReset, ReportsOnlyStreamsThatExist)
{
    Association::Config small;
    small.streams = 8;
    Network network{small, small};
    connect(network);
    std::uint32_t tag = 0;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (to_b) {
            tag = Packet::parse(packet)->header.verification_tag;
        }
        return true;
    };
    ASSERT_TRUE(network.a->send(0, 51, bufferOf("x"), {}, network.now));
    network.run(1s);
    const Streams real{3};
    ASSERT_TRUE(network.a->resetStreams(real, network.now));
    std::vector<std::byte> request;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (to_b && hasChunk(packet, ChunkType::ReConfig)) {
            request.assign(packet.begin(), packet.end());
            return false;
        }
        return true;
    };
    network.collect();
    ASSERT_FALSE(request.empty());
    const auto parsed = Packet::parse(request);
    const auto original = OutgoingResetRequest::parse(Tlv::parseAll(parsed->chunks.back().value)->front());

    std::vector<std::byte> fields;
    ByteWriter writer(fields);
    writer.write32(original->request_sequence);
    writer.write32(original->response_sequence);
    writer.write32(original->last_assigned_tsn);
    for (const auto stream : {5, 3, 9000, 5, 3}) {
        writer.write16(static_cast<std::uint16_t>(stream));
    }
    std::vector<std::byte> value;
    ByteWriter(value).writeTlv(static_cast<std::uint16_t>(ParameterType::OutgoingSsnResetRequest), fields);
    std::vector<std::byte> buffer;
    PacketBuilder builder(buffer, {.source_port = 5000, .destination_port = 5000, .verification_tag = tag}, 1200);
    builder.addChunk(ChunkType::ReConfig, 0, value);
    network.b->handlePacket(builder.build(), network.now);
    ASSERT_EQ(network.endpoint_b.incoming_resets.size(), 1U);
    EXPECT_EQ(network.endpoint_b.incoming_resets[0], (Streams{3, 5}));
}

TEST(StreamReset, GivesUpOnAPeerThatStaysInProgress)
{
    Association::Config config;
    config.max_retransmissions = 3;
    config.heartbeat_interval = {};
    Network network{config};
    connect(network);
    std::uint32_t tag = 0;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (!to_b) {
            tag = Packet::parse(packet)->header.verification_tag;
        }
        return true;
    };
    ASSERT_TRUE(network.b->send(0, 51, bufferOf("x"), {}, network.now));
    network.run(1s);
    ASSERT_NE(tag, 0U);

    std::vector<std::uint32_t> sequences;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (!to_b) {
            return true;
        }
        for (const auto &chunk : Packet::parse(packet)->chunks) {
            if (chunk.type == static_cast<std::uint8_t>(ChunkType::ReConfig)) {
                sequences.push_back(OutgoingResetRequest::parse(Tlv::parseAll(chunk.value)->front())->request_sequence);
            }
        }
        return false;
    };
    const Streams streams{1};
    ASSERT_TRUE(network.a->resetStreams(streams, network.now));
    for (auto answer = 0; answer < 6 && network.endpoint_a.outgoing_resets.empty(); ++answer) {
        ASSERT_TRUE(network.runUntil([&] { return sequences.size() > static_cast<std::size_t>(answer); }, 120s));
        std::vector<std::byte> fields;
        ByteWriter fields_writer(fields);
        fields_writer.write32(sequences.back());
        fields_writer.write32(static_cast<std::uint32_t>(ReconfigResult::InProgress));
        std::vector<std::byte> value;
        ByteWriter(value).writeTlv(static_cast<std::uint16_t>(ParameterType::ReconfigurationResponse), fields);
        std::vector<std::byte> buffer;
        PacketBuilder builder(buffer, {.source_port = 5000, .destination_port = 5000, .verification_tag = tag}, 1200);
        builder.addChunk(ChunkType::ReConfig, 0, value);
        network.a->handlePacket(builder.build(), network.now);
    }
    ASSERT_EQ(network.endpoint_a.outgoing_resets.size(), 1U);
    EXPECT_FALSE(network.endpoint_a.outgoing_resets[0].second);
    EXPECT_FALSE(network.endpoint_a.closed);
}

TEST(StreamReset, RefusesResetBeforeEstablishment)
{
    Network network;
    const Streams streams{1};
    EXPECT_FALSE(network.a->resetStreams(streams, network.now));
}

}  // namespace
}  // namespace endgate::rtc::sctp
