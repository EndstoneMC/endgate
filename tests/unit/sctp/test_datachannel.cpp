#include <chrono>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "network.h"

namespace endgate::rtc::sctp {
namespace {

using namespace std::chrono_literals;

const Reliability reliable{};
const Reliability unreliable{.unordered = true, .max_retransmits = 0, .max_packet_life_time = {}};

class DataChannelTest : public testing::Test {
protected:
    DataChannelTest()
        : network({.dtls_client = false, .remote_max_message_size = 262144},
                  {.dtls_client = true, .remote_max_message_size = 262144})
    {
    }

    void connect()
    {
        network.a->start(network.now);
        network.b->start(network.now);
        ASSERT_TRUE(network.runUntil(
            [&] {
                return network.a->state() == SctpTransport::State::Connected &&
                       network.b->state() == SctpTransport::State::Connected;
            },
            5s));
    }

    void openNetherNetChannels()
    {
        connect();
        reliable_stream = network.a->createDataChannel("ReliableDataChannel", "", reliable, 256, network.now);
        unreliable_stream = network.a->createDataChannel("UnreliableDataChannel", "", unreliable, 256, network.now);
        ASSERT_TRUE(reliable_stream);
        ASSERT_TRUE(unreliable_stream);
        ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.opened.size() == 2; }, 5s));
    }

    TransportNetwork network;
    std::optional<std::uint16_t> reliable_stream;
    std::optional<std::uint16_t> unreliable_stream;
};

TEST_F(DataChannelTest, ReportsConnectionState)
{
    connect();
    EXPECT_EQ(network.endpoint_a.states, std::vector{SctpTransport::State::Connected});
    EXPECT_EQ(network.endpoint_b.states, std::vector{SctpTransport::State::Connected});
    EXPECT_EQ(network.b->maxMessageSize(), 262144U);
}

TEST_F(DataChannelTest, ReceivesTheChannelsANetherNetClientOpens)
{
    openNetherNetChannels();
    EXPECT_EQ(*reliable_stream, 1);
    EXPECT_EQ(*unreliable_stream, 3);
    const auto &opened = network.endpoint_b.opened;
    EXPECT_EQ(opened[0].stream, 1);
    EXPECT_EQ(opened[0].label, "ReliableDataChannel");
    EXPECT_EQ(opened[0].reliability, reliable);
    EXPECT_EQ(opened[0].priority, 256);
    EXPECT_EQ(opened[1].stream, 3);
    EXPECT_EQ(opened[1].label, "UnreliableDataChannel");
    EXPECT_EQ(opened[1].reliability, unreliable);
    EXPECT_TRUE(network.endpoint_a.opened.empty());
    ASSERT_NE(network.b->channel(3), nullptr);
    EXPECT_EQ(network.b->channel(3)->label, "UnreliableDataChannel");
}

TEST_F(DataChannelTest, CarriesMessagesBothWaysOnBothReliabilities)
{
    openNetherNetChannels();
    ASSERT_TRUE(network.a->send(1, bufferOf("reliable up"), true, network.now));
    ASSERT_TRUE(network.a->send(3, bufferOf("unreliable up"), true, network.now));
    ASSERT_TRUE(network.b->send(1, bufferOf("reliable down"), false, network.now));
    ASSERT_TRUE(network.b->send(3, bufferOf("unreliable down"), true, network.now));
    ASSERT_TRUE(network.runUntil(
        [&] { return network.endpoint_a.messages.size() == 2 && network.endpoint_b.messages.size() == 2; }, 5s));

    std::set<std::string> up;
    for (const auto &message : network.endpoint_b.messages) {
        up.insert(message.label + ":" + message.payload);
        EXPECT_TRUE(message.binary);
    }
    EXPECT_EQ(up, (std::set<std::string>{"ReliableDataChannel:reliable up", "UnreliableDataChannel:unreliable up"}));
    std::set<std::string> down;
    for (const auto &message : network.endpoint_a.messages) {
        down.insert(message.label + ":" + message.payload + (message.binary ? ":binary" : ":string"));
    }
    EXPECT_EQ(down, (std::set<std::string>{"ReliableDataChannel:reliable down:string",
                                           "UnreliableDataChannel:unreliable down:binary"}));
}

TEST_F(DataChannelTest, SendsOrderedUntilTheChannelIsAcknowledged)
{
    connect();
    std::vector<std::uint8_t> flags;
    network.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (!to_b) {
            return true;
        }
        for (const auto &chunk : Packet::parse(packet)->chunks) {
            const auto data = chunk.type == 0 ? DataChunk::parse(chunk) : std::nullopt;
            if (data && data->ppid == static_cast<std::uint32_t>(Ppid::Binary)) {
                flags.push_back(data->flags & DataChunk::unordered_flag);
            }
        }
        return true;
    };
    const auto stream = network.a->createDataChannel("UnreliableDataChannel", "", unreliable, 256, network.now);
    ASSERT_TRUE(stream);
    ASSERT_TRUE(network.a->send(*stream, bufferOf("early"), true, network.now));
    network.run(1s);
    ASSERT_TRUE(network.a->send(*stream, bufferOf("late"), true, network.now));
    network.run(1s);
    EXPECT_EQ(flags, (std::vector<std::uint8_t>{0, DataChunk::unordered_flag}));
    EXPECT_EQ(network.endpoint_b.messages.size(), 2U);
}

TEST_F(DataChannelTest, OpensChannelsFromBothSides)
{
    openNetherNetChannels();
    const auto ours = network.b->createDataChannel("server", "proto", reliable, 512, network.now);
    ASSERT_TRUE(ours);
    EXPECT_EQ(*ours, 0);
    const auto next = network.b->createDataChannel("server 2", "", reliable, 512, network.now);
    EXPECT_EQ(next, 2);
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_a.opened.size() == 2; }, 5s));
    EXPECT_EQ(network.endpoint_a.opened[0].label, "server");
    EXPECT_EQ(network.endpoint_a.opened[0].protocol, "proto");
    ASSERT_TRUE(network.b->send(0, bufferOf("hello"), false, network.now));
    ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_a.messages.empty(); }, 5s));
    EXPECT_EQ(network.endpoint_a.messages[0].label, "server");
}

TEST_F(DataChannelTest, ClosesFromEitherSideAndReusesTheStream)
{
    openNetherNetChannels();
    network.b->closeDataChannel(1, network.now);
    EXPECT_FALSE(network.b->send(1, bufferOf("refused"), true, network.now));
    ASSERT_TRUE(network.runUntil(
        [&] { return network.endpoint_a.closed.size() == 1 && network.endpoint_b.closed.size() == 1; }, 5s));
    EXPECT_EQ(network.endpoint_a.closed[0].label, "ReliableDataChannel");
    EXPECT_EQ(network.endpoint_b.closed[0].label, "ReliableDataChannel");
    EXPECT_EQ(network.b->channel(1), nullptr);
    EXPECT_NE(network.b->channel(3), nullptr);

    network.a->closeDataChannel(3, network.now);
    ASSERT_TRUE(network.runUntil(
        [&] { return network.endpoint_a.closed.size() == 2 && network.endpoint_b.closed.size() == 2; }, 5s));

    const auto reopened = network.a->createDataChannel("again", "", reliable, 256, network.now);
    EXPECT_EQ(reopened, 1);
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.opened.size() == 3; }, 5s));
    ASSERT_TRUE(network.a->send(1, bufferOf("after reuse"), false, network.now));
    ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_b.messages.empty(); }, 5s));
    EXPECT_EQ(network.endpoint_b.messages[0].payload, "after reuse");
    EXPECT_EQ(network.endpoint_b.messages[0].label, "again");
}

TEST_F(DataChannelTest, DeliversQueuedMessagesBeforeClosing)
{
    openNetherNetChannels();
    for (auto i = 0; i < 20; ++i) {
        ASSERT_TRUE(network.a->send(1, bufferOf("m" + std::to_string(i)), false, network.now));
    }
    network.a->closeDataChannel(1, network.now);
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.closed.size() == 1; }, 5s));
    ASSERT_EQ(network.endpoint_b.messages.size(), 20U);
    EXPECT_EQ(network.endpoint_b.messages.back().payload, "m19");
}

TEST_F(DataChannelTest, CarriesEmptyMessages)
{
    openNetherNetChannels();
    ASSERT_TRUE(network.a->send(1, ByteBuf{}, false, network.now));
    ASSERT_TRUE(network.a->send(1, ByteBuf{}, true, network.now));
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.messages.size() == 2; }, 5s));
    EXPECT_EQ(network.endpoint_b.messages[0].payload, "");
    EXPECT_FALSE(network.endpoint_b.messages[0].binary);
    EXPECT_EQ(network.endpoint_b.messages[1].payload, "");
    EXPECT_TRUE(network.endpoint_b.messages[1].binary);
}

TEST_F(DataChannelTest, CarriesTheLargestMessageTheClientAdvertises)
{
    openNetherNetChannels();
    const std::string large(262144, 'n');
    ASSERT_TRUE(network.b->send(1, bufferOf(large), true, network.now));
    EXPECT_FALSE(network.b->send(1, bufferOf(large + "x"), true, network.now));
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_a.messages.size() == 1; }, 10s));
    EXPECT_EQ(network.endpoint_a.messages[0].payload, large);
}

TEST_F(DataChannelTest, ClosesEveryChannelWithTheAssociation)
{
    openNetherNetChannels();
    network.a->close(network.now);
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.closed.size() == 2; }, 5s));
    EXPECT_EQ(network.endpoint_b.states.back(), SctpTransport::State::Disconnected);
    EXPECT_EQ(network.endpoint_a.states.back(), SctpTransport::State::Disconnected);
    EXPECT_FALSE(network.a->send(1, bufferOf("gone"), false, network.now));
}

TEST_F(DataChannelTest, FailsEveryChannelOnAbort)
{
    openNetherNetChannels();
    network.b->abort(network.now);
    EXPECT_EQ(network.endpoint_b.closed.size(), 2U);
    EXPECT_EQ(network.endpoint_b.states.back(), SctpTransport::State::Failed);
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_a.closed.size() == 2; }, 5s));
    EXPECT_EQ(network.endpoint_a.states.back(), SctpTransport::State::Failed);
}

TEST_F(DataChannelTest, BoundsTheChannelsAPeerMayOpen)
{
    network.b.reset();
    network.b.emplace(network.endpoint_b, SctpTransport::Config{.dtls_client = true, .max_channels = 2});
    connect();
    for (auto i = 0; i < 4; ++i) {
        ASSERT_TRUE(network.a->createDataChannel("c" + std::to_string(i), "", reliable, 256, network.now));
    }
    network.run(2s);
    EXPECT_EQ(network.endpoint_b.opened.size(), 2U);
    EXPECT_EQ(network.endpoint_a.closed.size(), 2U);
}

TEST_F(DataChannelTest, ListenerMaySendFromInsideAMessage)
{
    openNetherNetChannels();
    network.endpoint_b.on_message = [&](const DataChannel &channel, const ByteBuf &message) {
        EXPECT_TRUE(network.b->send(channel.stream, ByteBuf::copyBuffer(std::vector<std::byte>(message.length())), true,
                                    network.now));
    };
    ASSERT_TRUE(network.a->send(1, bufferOf("echo me"), true, network.now));
    ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_a.messages.empty(); }, 5s));
    EXPECT_EQ(network.endpoint_a.messages[0].payload.size(), 7U);
}

class RawPeerTest : public testing::Test {
protected:
    RawPeerTest()
        : peer(raw, {.random = seeded(5)}),
          transport(server, {.dtls_client = true, .association = {.random = seeded(6)}})
    {
        peer.connect(now);
        transport.start(now);
        pump();
    }

    void pump()
    {
        for (auto round = 0; round < 10; ++round) {
            for (auto &packet : std::exchange(raw.outbox, {})) {
                transport.handlePacket(packet, now);
            }
            for (auto &packet : std::exchange(server.outbox, {})) {
                peer.handlePacket(packet, now);
            }
        }
    }

    void open(std::uint16_t stream, std::vector<std::byte> message)
    {
        ASSERT_TRUE(peer.send(stream, static_cast<std::uint32_t>(Ppid::Dcep), ByteBuf::copyBuffer(message), {}, now));
        pump();
    }

    std::chrono::steady_clock::time_point now{std::chrono::steady_clock::time_point{} + 1h};
    Endpoint raw;
    TransportEndpoint server;
    Association peer;
    SctpTransport transport;
};

TEST_F(RawPeerTest, AcknowledgesAValidOpen)
{
    ASSERT_EQ(transport.state(), SctpTransport::State::Connected);
    open(1, DataChannelOpen::of("ReliableDataChannel", "", reliable, 256).serialize());
    ASSERT_EQ(server.opened.size(), 1U);
    ASSERT_EQ(raw.messages.size(), 1U);
    EXPECT_EQ(raw.messages[0].ppid, static_cast<std::uint32_t>(Ppid::Dcep));
    EXPECT_EQ(text(raw.messages[0].payload), std::string(1, '\x02'));
    EXPECT_FALSE(raw.messages[0].unordered);
}

TEST_F(RawPeerTest, RefusesAnOpenOnOurParity)
{
    open(4, DataChannelOpen::of("even", "", reliable, 256).serialize());
    EXPECT_TRUE(server.opened.empty());
    EXPECT_TRUE(raw.messages.empty());
    ASSERT_EQ(raw.incoming_resets.size(), 1U);
    EXPECT_EQ(raw.incoming_resets[0], std::vector<std::uint16_t>{4});
}

TEST_F(RawPeerTest, RefusesMalformedOrUnknownOpen)
{
    auto unknown = DataChannelOpen::of("odd", "", reliable, 256).serialize();
    unknown[1] = std::byte{0x05};
    open(5, unknown);
    open(7, {std::byte{0x03}, std::byte{0x00}});
    EXPECT_TRUE(server.opened.empty());
    EXPECT_EQ(raw.incoming_resets.size(), 2U);
}

TEST_F(RawPeerTest, RefusesAnOpenOnAStreamInUse)
{
    open(1, DataChannelOpen::of("first", "", reliable, 256).serialize());
    open(1, DataChannelOpen::of("second", "", reliable, 256).serialize());
    ASSERT_EQ(server.opened.size(), 1U);
    EXPECT_EQ(raw.incoming_resets.size(), 1U);
}

TEST_F(RawPeerTest, RefusesUserDataOnAnUnopenedStream)
{
    ASSERT_TRUE(peer.send(9, static_cast<std::uint32_t>(Ppid::Binary), bufferOf("stray"), {}, now));
    pump();
    EXPECT_TRUE(server.messages.empty());
    ASSERT_EQ(raw.incoming_resets.size(), 1U);
    EXPECT_EQ(raw.incoming_resets[0], std::vector<std::uint16_t>{9});
}

TEST_F(RawPeerTest, ClosesChannelOnUnsupportedPpid)
{
    open(1, DataChannelOpen::of("chan", "", reliable, 256).serialize());
    ASSERT_TRUE(peer.send(1, static_cast<std::uint32_t>(Ppid::BinaryPartial), bufferOf("old"), {}, now));
    pump();
    EXPECT_TRUE(server.messages.empty());
    ASSERT_EQ(raw.incoming_resets.size(), 1U);
    const std::vector<std::uint16_t> streams{1};
    ASSERT_TRUE(peer.resetStreams(streams, now));
    pump();
    ASSERT_EQ(server.closed.size(), 1U);
    EXPECT_EQ(server.closed[0].label, "chan");
}

TEST_F(RawPeerTest, ListenerMayCloseTheChannelItIsHearingFrom)
{
    open(1, DataChannelOpen::of("chan", "", reliable, 256).serialize());
    std::string label;
    server.on_message = [&](const DataChannel &channel, const ByteBuf &) {
        transport.abort(now);
        transport.closeDataChannel(channel.stream, now);
        label = channel.label;
    };
    ASSERT_TRUE(peer.send(1, static_cast<std::uint32_t>(Ppid::Binary), bufferOf("x"), {}, now));
    pump();
    EXPECT_EQ(label, "chan");
    ASSERT_EQ(server.closed.size(), 1U);
    EXPECT_EQ(server.closed[0].label, "chan");
    EXPECT_EQ(transport.state(), SctpTransport::State::Failed);
}

TEST_F(RawPeerTest, ClosesChannelWhosePeerDeniesTheReset)
{
    open(1, DataChannelOpen::of("chan", "", reliable, 256).serialize());
    ASSERT_TRUE(peer.send(1, static_cast<std::uint32_t>(Ppid::Binary), bufferOf("x"), {}, now));
    const auto tag = Packet::parse(raw.outbox.front())->header.verification_tag;
    pump();

    transport.closeDataChannel(1, now);
    std::optional<std::uint32_t> sequence;
    for (const auto &packet : std::exchange(server.outbox, {})) {
        const auto parsed = Packet::parse(packet);
        for (const auto &chunk : parsed->chunks) {
            if (chunk.type == static_cast<std::uint8_t>(ChunkType::ReConfig)) {
                sequence = OutgoingResetRequest::parse(Tlv::parseAll(chunk.value)->front())->request_sequence;
            }
        }
    }
    ASSERT_TRUE(sequence);
    EXPECT_TRUE(server.closed.empty());

    std::vector<std::byte> fields;
    ByteWriter fields_writer(fields);
    fields_writer.write32(*sequence);
    fields_writer.write32(static_cast<std::uint32_t>(ReconfigResult::Denied));
    std::vector<std::byte> value;
    ByteWriter(value).writeTlv(static_cast<std::uint16_t>(ParameterType::ReconfigurationResponse), fields);
    std::vector<std::byte> buffer;
    PacketBuilder builder(buffer, {.source_port = 5000, .destination_port = 5000, .verification_tag = tag}, 1200);
    builder.addChunk(ChunkType::ReConfig, 0, value);
    transport.handlePacket(builder.build(), now);
    ASSERT_EQ(server.closed.size(), 1U);
    EXPECT_EQ(server.closed[0].label, "chan");
}

TEST_F(RawPeerTest, RefusesOversizedLabel)
{
    open(1, DataChannelOpen::of(std::string(1025, 'l'), "", reliable, 256).serialize());
    EXPECT_TRUE(server.opened.empty());
    EXPECT_EQ(raw.incoming_resets.size(), 1U);
}

TEST(DataChannelParity, RefusesAPeerOnTheSameDtlsRole)
{
    TransportNetwork network({.dtls_client = true}, {.dtls_client = true});
    network.a->start(network.now);
    network.b->start(network.now);
    ASSERT_TRUE(network.runUntil([&] { return network.a->state() == SctpTransport::State::Connected; }, 5s));
    ASSERT_EQ(network.a->createDataChannel("even", "", reliable, 256, network.now), 0);
    ASSERT_TRUE(network.runUntil([&] { return !network.endpoint_a.closed.empty(); }, 5s));
    EXPECT_TRUE(network.endpoint_b.opened.empty());
    EXPECT_EQ(network.endpoint_a.closed[0].label, "even");
}

TEST(DataChannelTransfer, KeepsReliableTrafficWhileUnreliableIsLost)
{
    TransportNetwork network({.dtls_client = false, .remote_max_message_size = 262144},
                             {.dtls_client = true, .remote_max_message_size = 262144}, 7);
    network.a->start(network.now);
    network.b->start(network.now);
    ASSERT_TRUE(network.runUntil([&] { return network.b->state() == SctpTransport::State::Connected; }, 5s));
    ASSERT_TRUE(network.runUntil([&] { return network.a->state() == SctpTransport::State::Connected; }, 5s));
    ASSERT_EQ(network.a->createDataChannel("ReliableDataChannel", "", reliable, 256, network.now), 1);
    ASSERT_EQ(network.a->createDataChannel("UnreliableDataChannel", "", unreliable, 256, network.now), 3);
    ASSERT_TRUE(network.runUntil([&] { return network.endpoint_b.opened.size() == 2; }, 5s));

    network.impairment = {.loss = 0.1, .duplication = 0.02, .reordering = 0.1};
    for (auto i = 0; i < 300; ++i) {
        ASSERT_TRUE(network.a->send(1, bufferOf("r" + std::to_string(i)), true, network.now));
        ASSERT_TRUE(network.a->send(3, bufferOf("u" + std::to_string(i)), true, network.now));
        ASSERT_TRUE(network.b->send(1, bufferOf("d" + std::to_string(i)), true, network.now));
        network.run(3ms);
    }
    const auto count = [](const std::vector<TransportEndpoint::Received> &messages, std::uint16_t stream) {
        return std::ranges::count_if(messages, [&](const auto &message) { return message.stream == stream; });
    };
    ASSERT_TRUE(network.runUntil(
        [&] { return count(network.endpoint_b.messages, 1) == 300 && count(network.endpoint_a.messages, 1) == 300; },
        120s));
    auto next = 0;
    for (const auto &message : network.endpoint_b.messages) {
        if (message.stream == 1) {
            EXPECT_EQ(message.payload, "r" + std::to_string(next++));
        }
    }
    EXPECT_LT(count(network.endpoint_b.messages, 3), 300);
    EXPECT_GT(count(network.endpoint_b.messages, 3), 200);
    EXPECT_EQ(network.a->state(), SctpTransport::State::Connected);
}

}  // namespace
}  // namespace endgate::rtc::sctp
