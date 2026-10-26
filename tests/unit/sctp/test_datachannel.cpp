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

const Reliability RELIABLE{};
const Reliability UNRELIABLE{.unordered = true, .max_retransmits = 0, .max_packet_life_time = {}};

class DataChannelTest : public testing::Test {
protected:
    DataChannelTest()
        : network_({.dtls_client = false, .remote_max_message_size = 262144},
                   {.dtls_client = true, .remote_max_message_size = 262144})
    {
    }

    void connect()
    {
        network_.a->start(network_.now);
        network_.b->start(network_.now);
        ASSERT_TRUE(network_.runUntil(
            [&] {
                return network_.a->state() == SctpTransport::State::Connected &&
                       network_.b->state() == SctpTransport::State::Connected;
            },
            5s));
    }

    void openNetherNetChannels()
    {
        connect();
        reliable_stream_ = network_.a->createDataChannel("ReliableDataChannel", "", RELIABLE, 256, network_.now);
        unreliable_stream_ = network_.a->createDataChannel("UnreliableDataChannel", "", UNRELIABLE, 256, network_.now);
        ASSERT_TRUE(reliable_stream_);
        ASSERT_TRUE(unreliable_stream_);
        ASSERT_TRUE(network_.runUntil([&] { return network_.endpoint_b.opened.size() == 2; }, 5s));
    }

    TransportNetwork network_;
    std::optional<std::uint16_t> reliable_stream_;
    std::optional<std::uint16_t> unreliable_stream_;
};

TEST_F(DataChannelTest, ReportsConnectionState)
{
    connect();
    EXPECT_EQ(network_.endpoint_a.states, std::vector{SctpTransport::State::Connected});
    EXPECT_EQ(network_.endpoint_b.states, std::vector{SctpTransport::State::Connected});
    EXPECT_EQ(network_.b->maxMessageSize(), 262144U);
}

TEST_F(DataChannelTest, ReceivesTheChannelsANetherNetClientOpens)
{
    openNetherNetChannels();
    EXPECT_EQ(*reliable_stream_, 1);
    EXPECT_EQ(*unreliable_stream_, 3);
    const auto &opened = network_.endpoint_b.opened;
    EXPECT_EQ(opened[0].stream, 1);
    EXPECT_EQ(opened[0].label, "ReliableDataChannel");
    EXPECT_EQ(opened[0].reliability, RELIABLE);
    EXPECT_EQ(opened[0].priority, 256);
    EXPECT_EQ(opened[1].stream, 3);
    EXPECT_EQ(opened[1].label, "UnreliableDataChannel");
    EXPECT_EQ(opened[1].reliability, UNRELIABLE);
    EXPECT_TRUE(network_.endpoint_a.opened.empty());
    ASSERT_NE(network_.b->channel(3), nullptr);
    EXPECT_EQ(network_.b->channel(3)->label, "UnreliableDataChannel");
}

TEST_F(DataChannelTest, CarriesMessagesBothWaysOnBothReliabilities)
{
    openNetherNetChannels();
    ASSERT_TRUE(network_.a->send(1, bufferOf("reliable up"), true, network_.now));
    ASSERT_TRUE(network_.a->send(3, bufferOf("unreliable up"), true, network_.now));
    ASSERT_TRUE(network_.b->send(1, bufferOf("reliable down"), false, network_.now));
    ASSERT_TRUE(network_.b->send(3, bufferOf("unreliable down"), true, network_.now));
    ASSERT_TRUE(network_.runUntil(
        [&] { return network_.endpoint_a.messages.size() == 2 && network_.endpoint_b.messages.size() == 2; }, 5s));

    std::set<std::string> up;
    for (const auto &message : network_.endpoint_b.messages) {
        up.insert(message.label + ":" + message.payload);
        EXPECT_TRUE(message.binary);
    }
    EXPECT_EQ(up, (std::set<std::string>{"ReliableDataChannel:reliable up", "UnreliableDataChannel:unreliable up"}));
    std::set<std::string> down;
    for (const auto &message : network_.endpoint_a.messages) {
        down.insert(message.label + ":" + message.payload + (message.binary ? ":binary" : ":string"));
    }
    EXPECT_EQ(down, (std::set<std::string>{"ReliableDataChannel:reliable down:string",
                                           "UnreliableDataChannel:unreliable down:binary"}));
}

TEST_F(DataChannelTest, SendsOrderedUntilTheChannelIsAcknowledged)
{
    connect();
    std::vector<std::uint8_t> flags;
    network_.filter = [&](std::span<const std::byte> packet, bool to_b) {
        if (!to_b) {
            return true;
        }
        for (const auto &chunk : Packet::parse(packet)->chunks) {
            const auto data = chunk.type == 0 ? DataChunk::parse(chunk) : std::nullopt;
            if (data && data->ppid == static_cast<std::uint32_t>(Ppid::Binary)) {
                flags.push_back(data->flags & DataChunk::UNORDERED_FLAG);
            }
        }
        return true;
    };
    const auto stream = network_.a->createDataChannel("UnreliableDataChannel", "", UNRELIABLE, 256, network_.now);
    ASSERT_TRUE(stream);
    ASSERT_TRUE(network_.a->send(*stream, bufferOf("early"), true, network_.now));
    network_.run(1s);
    ASSERT_TRUE(network_.a->send(*stream, bufferOf("late"), true, network_.now));
    network_.run(1s);
    EXPECT_EQ(flags, (std::vector<std::uint8_t>{0, DataChunk::UNORDERED_FLAG}));
    EXPECT_EQ(network_.endpoint_b.messages.size(), 2U);
}

TEST_F(DataChannelTest, OpensChannelsFromBothSides)
{
    openNetherNetChannels();
    const auto ours = network_.b->createDataChannel("server", "proto", RELIABLE, 512, network_.now);
    ASSERT_TRUE(ours);
    EXPECT_EQ(*ours, 0);
    const auto next = network_.b->createDataChannel("server 2", "", RELIABLE, 512, network_.now);
    EXPECT_EQ(next, 2);
    ASSERT_TRUE(network_.runUntil([&] { return network_.endpoint_a.opened.size() == 2; }, 5s));
    EXPECT_EQ(network_.endpoint_a.opened[0].label, "server");
    EXPECT_EQ(network_.endpoint_a.opened[0].protocol, "proto");
    ASSERT_TRUE(network_.b->send(0, bufferOf("hello"), false, network_.now));
    ASSERT_TRUE(network_.runUntil([&] { return !network_.endpoint_a.messages.empty(); }, 5s));
    EXPECT_EQ(network_.endpoint_a.messages[0].label, "server");
}

TEST_F(DataChannelTest, ClosesFromEitherSideAndReusesTheStream)
{
    openNetherNetChannels();
    network_.b->closeDataChannel(1, network_.now);
    EXPECT_FALSE(network_.b->send(1, bufferOf("refused"), true, network_.now));
    ASSERT_TRUE(network_.runUntil(
        [&] { return network_.endpoint_a.closed.size() == 1 && network_.endpoint_b.closed.size() == 1; }, 5s));
    EXPECT_EQ(network_.endpoint_a.closed[0].label, "ReliableDataChannel");
    EXPECT_EQ(network_.endpoint_b.closed[0].label, "ReliableDataChannel");
    EXPECT_EQ(network_.b->channel(1), nullptr);
    EXPECT_NE(network_.b->channel(3), nullptr);

    network_.a->closeDataChannel(3, network_.now);
    ASSERT_TRUE(network_.runUntil(
        [&] { return network_.endpoint_a.closed.size() == 2 && network_.endpoint_b.closed.size() == 2; }, 5s));

    const auto reopened = network_.a->createDataChannel("again", "", RELIABLE, 256, network_.now);
    EXPECT_EQ(reopened, 1);
    ASSERT_TRUE(network_.runUntil([&] { return network_.endpoint_b.opened.size() == 3; }, 5s));
    ASSERT_TRUE(network_.a->send(1, bufferOf("after reuse"), false, network_.now));
    ASSERT_TRUE(network_.runUntil([&] { return !network_.endpoint_b.messages.empty(); }, 5s));
    EXPECT_EQ(network_.endpoint_b.messages[0].payload, "after reuse");
    EXPECT_EQ(network_.endpoint_b.messages[0].label, "again");
}

TEST_F(DataChannelTest, DeliversQueuedMessagesBeforeClosing)
{
    openNetherNetChannels();
    for (auto i = 0; i < 20; ++i) {
        ASSERT_TRUE(network_.a->send(1, bufferOf("m" + std::to_string(i)), false, network_.now));
    }
    network_.a->closeDataChannel(1, network_.now);
    ASSERT_TRUE(network_.runUntil([&] { return network_.endpoint_b.closed.size() == 1; }, 5s));
    ASSERT_EQ(network_.endpoint_b.messages.size(), 20U);
    EXPECT_EQ(network_.endpoint_b.messages.back().payload, "m19");
}

TEST_F(DataChannelTest, CarriesEmptyMessages)
{
    openNetherNetChannels();
    ASSERT_TRUE(network_.a->send(1, ByteBuf{}, false, network_.now));
    ASSERT_TRUE(network_.a->send(1, ByteBuf{}, true, network_.now));
    ASSERT_TRUE(network_.runUntil([&] { return network_.endpoint_b.messages.size() == 2; }, 5s));
    EXPECT_EQ(network_.endpoint_b.messages[0].payload, "");
    EXPECT_FALSE(network_.endpoint_b.messages[0].binary);
    EXPECT_EQ(network_.endpoint_b.messages[1].payload, "");
    EXPECT_TRUE(network_.endpoint_b.messages[1].binary);
}

TEST_F(DataChannelTest, CarriesTheLargestMessageTheClientAdvertises)
{
    openNetherNetChannels();
    const std::string large(262144, 'n');
    ASSERT_TRUE(network_.b->send(1, bufferOf(large), true, network_.now));
    EXPECT_FALSE(network_.b->send(1, bufferOf(large + "x"), true, network_.now));
    ASSERT_TRUE(network_.runUntil([&] { return network_.endpoint_a.messages.size() == 1; }, 10s));
    EXPECT_EQ(network_.endpoint_a.messages[0].payload, large);
}

TEST_F(DataChannelTest, ClosesEveryChannelWithTheAssociation)
{
    openNetherNetChannels();
    network_.a->close(network_.now);
    ASSERT_TRUE(network_.runUntil([&] { return network_.endpoint_b.closed.size() == 2; }, 5s));
    EXPECT_EQ(network_.endpoint_b.states.back(), SctpTransport::State::Disconnected);
    EXPECT_EQ(network_.endpoint_a.states.back(), SctpTransport::State::Disconnected);
    EXPECT_FALSE(network_.a->send(1, bufferOf("gone"), false, network_.now));
}

TEST_F(DataChannelTest, FailsEveryChannelOnAbort)
{
    openNetherNetChannels();
    network_.b->abort(network_.now);
    EXPECT_EQ(network_.endpoint_b.closed.size(), 2U);
    EXPECT_EQ(network_.endpoint_b.states.back(), SctpTransport::State::Failed);
    ASSERT_TRUE(network_.runUntil([&] { return network_.endpoint_a.closed.size() == 2; }, 5s));
    EXPECT_EQ(network_.endpoint_a.states.back(), SctpTransport::State::Failed);
}

TEST_F(DataChannelTest, BoundsTheChannelsAPeerMayOpen)
{
    network_.b.reset();
    network_.b.emplace(network_.endpoint_b, SctpTransport::Config{.dtls_client = true, .max_channels = 2});
    connect();
    for (auto i = 0; i < 4; ++i) {
        ASSERT_TRUE(network_.a->createDataChannel("c" + std::to_string(i), "", RELIABLE, 256, network_.now));
    }
    network_.run(2s);
    EXPECT_EQ(network_.endpoint_b.opened.size(), 2U);
    EXPECT_EQ(network_.endpoint_a.closed.size(), 2U);
}

TEST_F(DataChannelTest, ListenerMaySendFromInsideAMessage)
{
    openNetherNetChannels();
    network_.endpoint_b.on_message = [&](const DataChannel &channel, const ByteBuf &message) {
        EXPECT_TRUE(network_.b->send(channel.stream, ByteBuf::copyBuffer(std::vector<std::byte>(message.length())),
                                     true, network_.now));
    };
    ASSERT_TRUE(network_.a->send(1, bufferOf("echo me"), true, network_.now));
    ASSERT_TRUE(network_.runUntil([&] { return !network_.endpoint_a.messages.empty(); }, 5s));
    EXPECT_EQ(network_.endpoint_a.messages[0].payload.size(), 7U);
}

class RawPeerTest : public testing::Test {
protected:
    RawPeerTest()
        : peer_(raw_, {.random = seeded(5)}),
          transport_(server_, {.dtls_client = true, .association = {.random = seeded(6)}})
    {
        peer_.connect(now_);
        transport_.start(now_);
        pump();
    }

    void pump()
    {
        for (auto round = 0; round < 10; ++round) {
            for (auto &packet : std::exchange(raw_.outbox, {})) {
                transport_.handlePacket(packet, now_);
            }
            for (auto &packet : std::exchange(server_.outbox, {})) {
                peer_.handlePacket(packet, now_);
            }
        }
    }

    void open(std::uint16_t stream, std::vector<std::byte> message)
    {
        ASSERT_TRUE(peer_.send(stream, static_cast<std::uint32_t>(Ppid::Dcep), ByteBuf::copyBuffer(message), {}, now_));
        pump();
    }

    std::chrono::steady_clock::time_point now_{std::chrono::steady_clock::time_point{} + 1h};
    Endpoint raw_;
    TransportEndpoint server_;
    Association peer_;
    SctpTransport transport_;
};

TEST_F(RawPeerTest, AcknowledgesAValidOpen)
{
    ASSERT_EQ(transport_.state(), SctpTransport::State::Connected);
    open(1, DataChannelOpen::of("ReliableDataChannel", "", RELIABLE, 256).serialize());
    ASSERT_EQ(server_.opened.size(), 1U);
    ASSERT_EQ(raw_.messages.size(), 1U);
    EXPECT_EQ(raw_.messages[0].ppid, static_cast<std::uint32_t>(Ppid::Dcep));
    EXPECT_EQ(text(raw_.messages[0].payload), std::string(1, '\x02'));
    EXPECT_FALSE(raw_.messages[0].unordered);
}

TEST_F(RawPeerTest, RefusesAnOpenOnOurParity)
{
    open(4, DataChannelOpen::of("even", "", RELIABLE, 256).serialize());
    EXPECT_TRUE(server_.opened.empty());
    EXPECT_TRUE(raw_.messages.empty());
    ASSERT_EQ(raw_.incoming_resets.size(), 1U);
    EXPECT_EQ(raw_.incoming_resets[0], std::vector<std::uint16_t>{4});
}

TEST_F(RawPeerTest, RefusesMalformedOrUnknownOpen)
{
    auto unknown = DataChannelOpen::of("odd", "", RELIABLE, 256).serialize();
    unknown[1] = std::byte{0x05};
    open(5, unknown);
    open(7, {std::byte{0x03}, std::byte{0x00}});
    EXPECT_TRUE(server_.opened.empty());
    EXPECT_EQ(raw_.incoming_resets.size(), 2U);
}

TEST_F(RawPeerTest, RefusesAnOpenOnAStreamInUse)
{
    open(1, DataChannelOpen::of("first", "", RELIABLE, 256).serialize());
    open(1, DataChannelOpen::of("second", "", RELIABLE, 256).serialize());
    ASSERT_EQ(server_.opened.size(), 1U);
    EXPECT_EQ(raw_.incoming_resets.size(), 1U);
}

TEST_F(RawPeerTest, RefusesUserDataOnAnUnopenedStream)
{
    ASSERT_TRUE(peer_.send(9, static_cast<std::uint32_t>(Ppid::Binary), bufferOf("stray"), {}, now_));
    pump();
    EXPECT_TRUE(server_.messages.empty());
    ASSERT_EQ(raw_.incoming_resets.size(), 1U);
    EXPECT_EQ(raw_.incoming_resets[0], std::vector<std::uint16_t>{9});
}

TEST_F(RawPeerTest, ClosesChannelOnUnsupportedPpid)
{
    open(1, DataChannelOpen::of("chan", "", RELIABLE, 256).serialize());
    ASSERT_TRUE(peer_.send(1, static_cast<std::uint32_t>(Ppid::BinaryPartial), bufferOf("old"), {}, now_));
    pump();
    EXPECT_TRUE(server_.messages.empty());
    ASSERT_EQ(raw_.incoming_resets.size(), 1U);
    const std::vector<std::uint16_t> streams{1};
    ASSERT_TRUE(peer_.resetStreams(streams, now_));
    pump();
    ASSERT_EQ(server_.closed.size(), 1U);
    EXPECT_EQ(server_.closed[0].label, "chan");
}

TEST_F(RawPeerTest, ListenerMayCloseTheChannelItIsHearingFrom)
{
    open(1, DataChannelOpen::of("chan", "", RELIABLE, 256).serialize());
    std::string label;
    server_.on_message = [&](const DataChannel &channel, const ByteBuf &) {
        transport_.abort(now_);
        transport_.closeDataChannel(channel.stream, now_);
        label = channel.label;
    };
    ASSERT_TRUE(peer_.send(1, static_cast<std::uint32_t>(Ppid::Binary), bufferOf("x"), {}, now_));
    pump();
    EXPECT_EQ(label, "chan");
    ASSERT_EQ(server_.closed.size(), 1U);
    EXPECT_EQ(server_.closed[0].label, "chan");
    EXPECT_EQ(transport_.state(), SctpTransport::State::Failed);
}

TEST_F(RawPeerTest, ClosesChannelWhosePeerDeniesTheReset)
{
    open(1, DataChannelOpen::of("chan", "", RELIABLE, 256).serialize());
    ASSERT_TRUE(peer_.send(1, static_cast<std::uint32_t>(Ppid::Binary), bufferOf("x"), {}, now_));
    const auto tag = Packet::parse(raw_.outbox.front())->header.verification_tag;
    pump();

    transport_.closeDataChannel(1, now_);
    std::optional<std::uint32_t> sequence;
    for (const auto &packet : std::exchange(server_.outbox, {})) {
        const auto parsed = Packet::parse(packet);
        for (const auto &chunk : parsed->chunks) {
            if (chunk.type == static_cast<std::uint8_t>(ChunkType::ReConfig)) {
                sequence = OutgoingResetRequest::parse(Tlv::parseAll(chunk.value)->front())->request_sequence;
            }
        }
    }
    ASSERT_TRUE(sequence);
    EXPECT_TRUE(server_.closed.empty());

    std::vector<std::byte> fields;
    ByteWriter fields_writer(fields);
    fields_writer.write32(*sequence);
    fields_writer.write32(static_cast<std::uint32_t>(ReconfigResult::Denied));
    std::vector<std::byte> value;
    ByteWriter(value).writeTlv(static_cast<std::uint16_t>(ParameterType::ReconfigurationResponse), fields);
    std::vector<std::byte> buffer;
    PacketBuilder builder(buffer, {.source_port = 5000, .destination_port = 5000, .verification_tag = tag}, 1200);
    builder.addChunk(ChunkType::ReConfig, 0, value);
    transport_.handlePacket(builder.build(), now_);
    ASSERT_EQ(server_.closed.size(), 1U);
    EXPECT_EQ(server_.closed[0].label, "chan");
}

TEST_F(RawPeerTest, RefusesOversizedLabel)
{
    open(1, DataChannelOpen::of(std::string(1025, 'l'), "", RELIABLE, 256).serialize());
    EXPECT_TRUE(server_.opened.empty());
    EXPECT_EQ(raw_.incoming_resets.size(), 1U);
}

TEST(DataChannelParity, RefusesAPeerOnTheSameDtlsRole)
{
    TransportNetwork network({.dtls_client = true}, {.dtls_client = true});
    network.a->start(network.now);
    network.b->start(network.now);
    ASSERT_TRUE(network.runUntil([&] { return network.a->state() == SctpTransport::State::Connected; }, 5s));
    ASSERT_EQ(network.a->createDataChannel("even", "", RELIABLE, 256, network.now), 0);
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
    ASSERT_EQ(network.a->createDataChannel("ReliableDataChannel", "", RELIABLE, 256, network.now), 1);
    ASSERT_EQ(network.a->createDataChannel("UnreliableDataChannel", "", UNRELIABLE, 256, network.now), 3);
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
