#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/network/raknet/connection.h"
#include "endgate/network/raknet/offline.h"
#include "simulation.h"
#include "support.h"

namespace endgate::raknet {
namespace {

using namespace std::chrono_literals;
using test::fromHex;
using test::Simulation;
using test::toHex;

const SystemAddress CLIENT_ADDRESS{asio::ip::make_address("10.0.0.2"), 50000};
const SystemAddress SERVER_ADDRESS{asio::ip::make_address("10.0.0.1"), 19132};

struct Events : Connection::Listener {
    Events(Simulation &simulation, std::size_t index) : simulation(simulation), index(index) {}

    void sendDatagram(ByteBufView datagram) override
    {
        const auto bytes = datagram.nioBuffer();
        datagrams.emplace_back(bytes.begin(), bytes.end());
        simulation.transmit(index, bytes);
    }

    bool sendMtuProbe(ByteBufView datagram) override
    {
        probes.push_back(datagram.readableBytes());
        probe_times.push_back(simulation.now());
        if (datagram.readableBytes() > path_mtu) {
            return false;
        }
        sendDatagram(datagram);
        return true;
    }

    void onConnected() override { ++connected; }

    void onMessage(ByteBufView message) override
    {
        const auto bytes = message.nioBuffer();
        messages.emplace_back(bytes.begin(), bytes.end());
        if (on_message) {
            on_message();
        }
    }

    void onDisconnected(DisconnectReason reason) override { disconnected.push_back(reason); }

    void onReliabilityLayerNotification(std::string_view message, bool is_error) override
    {
        notifications.emplace_back(message);
        errors += is_error ? 1 : 0;
    }

    Simulation &simulation;
    std::size_t index;
    std::size_t path_mtu{65535};
    std::vector<std::vector<std::byte>> datagrams;
    std::vector<std::size_t> probes;
    std::vector<TimePoint> probe_times;
    int connected{0};
    std::vector<std::vector<std::byte>> messages;
    std::vector<DisconnectReason> disconnected;
    std::vector<std::string> notifications;
    int errors{0};
    std::function<void()> on_message;
};

struct Server : OfflineHandler::Listener {
    explicit Server(Simulation &simulation) : simulation(simulation), events(simulation, 1), offline(*this, 0x5E5E) {}

    void sendTo(const SystemAddress &address, ByteBufView datagram) override
    {
        EXPECT_EQ(address, CLIENT_ADDRESS);
        const auto bytes = datagram.nioBuffer();
        offline_replies.emplace_back(bytes.begin(), bytes.end());
        simulation.transmit(1, bytes);
    }

    void receive(ByteBufView datagram, TimePoint now)
    {
        const auto id = static_cast<MessageId>(datagram.getUnsignedByte(datagram.readerIndex()));
        const bool offline_message = isOfflineMessage(datagram);
        if (offline_message && (id == MessageId::UnconnectedPing || id == MessageId::UnconnectedPingOpenConnections ||
                                id == MessageId::OpenConnectionRequest1)) {
            static_cast<void>(offline.handle(datagram, CLIENT_ADDRESS, accepting));
            return;
        }
        if (connection && !connection->isClosed()) {
            connection->handleDatagram(datagram, now);
            return;
        }
        if (!offline_message || id != MessageId::OpenConnectionRequest2) {
            return;
        }
        const auto request = offline.handle(datagram, CLIENT_ADDRESS, accepting);
        if (!request) {
            return;
        }
        if (!accepting) {
            offline.reject(CLIENT_ADDRESS, MessageId::NoFreeIncomingConnections);
            return;
        }
        connection = std::make_unique<Connection>(
            events, ConnectionConfiguration{.guid = configuration.guid, .password = configuration.password.copy()},
            CLIENT_ADDRESS, *request, 0, now);
        connection->handleDatagram(datagram, now);
    }

    Simulation &simulation;
    Events events;
    OfflineHandler offline;
    ConnectionConfiguration configuration{.guid = 0x5E5E};
    bool accepting{true};
    std::unique_ptr<Connection> connection;
    std::vector<std::vector<std::byte>> offline_replies;
};

struct Handshake {
    explicit Handshake(ConnectionConfiguration client_configuration = {.guid = 0xC1C1}, std::uint32_t seed = 1)
        : simulation(seed), client_events(simulation, 0), server(simulation),
          client(client_events, std::move(client_configuration), SERVER_ADDRESS, simulation.now())
    {
        simulation.attach(
            0, {.receive = [this](std::span<const std::byte> datagram,
                                  TimePoint now) { client.handleDatagram(Unpooled::wrappedBuffer(datagram), now); },
                .next_timeout = [this] { return client.nextTimeout(); },
                .timeout = [this](TimePoint now) { client.handleTimeout(now); }});
        simulation.attach(
            1,
            {.receive = [this](std::span<const std::byte> datagram,
                               TimePoint now) { server.receive(Unpooled::wrappedBuffer(datagram), now); },
             .next_timeout =
                 [this] { return server.connection ? server.connection->nextTimeout() : std::optional<TimePoint>{}; },
             .timeout = [this](TimePoint now) { server.connection->handleTimeout(now); }});
    }

    bool connect()
    {
        return simulation.runUntil(
            [this] { return client.isConnected() && server.connection && server.connection->isConnected(); }, 10s);
    }

    Simulation simulation;
    Events client_events;
    Server server;
    Connection client;
};

std::vector<std::byte> payload(std::size_t size, std::uint8_t seed = 0)
{
    std::vector<std::byte> bytes(size);
    bytes[0] = std::byte{0xFE};
    for (std::size_t i = 1; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 7 + seed) & 0xFF);
    }
    return bytes;
}

TEST(Connection, HandshakeCompletesInBothRoles)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    EXPECT_EQ(handshake.client_events.connected, 1);
    EXPECT_EQ(handshake.server.events.connected, 1);
    EXPECT_EQ(handshake.client.getGuid(), 0x5E5EU);
    EXPECT_EQ(handshake.server.connection->getGuid(), 0xC1C1U);
    EXPECT_EQ(handshake.client.getMtuSize(), 1600U);
    EXPECT_EQ(handshake.client.getExternalId(), CLIENT_ADDRESS);
    EXPECT_EQ(handshake.server.connection->getExternalId(), SERVER_ADDRESS);
    EXPECT_TRUE(handshake.client.weInitiatedTheConnection());
    EXPECT_FALSE(handshake.server.connection->weInitiatedTheConnection());
    EXPECT_EQ(handshake.client_events.errors + handshake.server.events.errors, 0);
}

TEST(Connection, MessagesCrossBothWaysAfterTheHandshake)
{
    Handshake handshake{{.guid = 0xC1C1}, 9};
    ASSERT_TRUE(handshake.connect());
    handshake.simulation.setConditions(0, {.loss = 0.1, .duplicate = 0.05, .reorder = 0.1});
    handshake.simulation.setConditions(1, {.loss = 0.1, .duplicate = 0.05, .reorder = 0.1});
    const auto up = payload(300000, 1);
    const auto down = payload(70000, 2);
    ASSERT_TRUE(handshake.client.send(Unpooled::copiedBuffer(up), PacketPriority::High,
                                      PacketReliability::ReliableOrdered, 0, handshake.simulation.now()));
    ASSERT_TRUE(handshake.server.connection->send(Unpooled::copiedBuffer(down), PacketPriority::High,
                                                  PacketReliability::ReliableOrdered, 0, handshake.simulation.now()));
    ASSERT_TRUE(handshake.simulation.runUntil(
        [&] { return !handshake.client_events.messages.empty() && !handshake.server.events.messages.empty(); }, 60s));
    EXPECT_EQ(handshake.server.events.messages[0], up);
    EXPECT_EQ(handshake.client_events.messages[0], down);
}

TEST(Connection, ProbesFallFromThe1600To1200To576ByteMtu)
{
    Handshake handshake;
    handshake.simulation.setConditions(0, {.loss = 1.0});
    ASSERT_TRUE(handshake.simulation.runUntil([&] { return !handshake.client_events.disconnected.empty(); }, 10s));
    EXPECT_EQ(handshake.client_events.disconnected,
              std::vector<DisconnectReason>{DisconnectReason::ConnectionAttemptFailed});
    EXPECT_EQ(handshake.client_events.probes,
              (std::vector<std::size_t>{1572, 1572, 1572, 1572, 1172, 1172, 1172, 1172, 548, 548, 548, 548, 548}));
    const auto &times = handshake.client_events.probe_times;
    for (std::size_t i = 1; i < times.size(); ++i) {
        EXPECT_EQ(times[i] - times[i - 1], 500ms);
    }
    EXPECT_EQ(handshake.simulation.now() - times.front(), 6500ms);
}

TEST(Connection, ProbeTooLargeForThePathFallsBackAtOnce)
{
    Handshake handshake;
    handshake.client_events.path_mtu = 1500 - UDP_HEADER_SIZE;
    ASSERT_TRUE(handshake.connect());
    EXPECT_EQ(handshake.client_events.probes, (std::vector<std::size_t>{1572, 1172}));
    EXPECT_EQ(handshake.client.getMtuSize(), 1200U);
    EXPECT_EQ(handshake.server.connection->getMtuSize(), 1200U);
}

TEST(Connection, ServerAnswersOpenConnectionRequestsLikeTheReference)
{
    Simulation simulation;
    Server server{simulation};
    server.receive(fromHex("05"
                           "00ffff00fefefefefdfdfdfd12345678"
                           "0b" +
                           std::string(2 * 400, '0')),
                   simulation.now());
    server.receive(fromHex("05"
                           "00ffff00fefefefefdfdfdfd12345678"
                           "0a" +
                           std::string(2 * 400, '0')),
                   simulation.now());
    ASSERT_EQ(server.offline_replies.size(), 2U);
    EXPECT_EQ(toHex(server.offline_replies[0]), "06"
                                                "00ffff00fefefefefdfdfdfd12345678"
                                                "0000000000005e5e"
                                                "00"
                                                "01be");
    EXPECT_EQ(toHex(server.offline_replies[1]), "190b"
                                                "00ffff00fefefefefdfdfdfd12345678"
                                                "0000000000005e5e");

    server.receive(fromHex("05"
                           "00ffff00fefefefefdfdfdfd12345678"
                           "0b" +
                           std::string(2 * 2000, '0')),
                   simulation.now());
    EXPECT_EQ(toHex(server.offline_replies[2]).substr(52), "0640");
}

TEST(Connection, RepeatedOpenConnectionRequest2IsAnsweredAgainOrRefused)
{
    Simulation simulation;
    Server server{simulation};
    const auto request = fromHex("07"
                                 "00ffff00fefefefefdfdfdfd12345678"
                                 "04f5fffffe4abc"
                                 "04b0"
                                 "000000000000c1c1");
    server.receive(request, simulation.now());
    server.receive(request, simulation.now());
    server.receive(fromHex("07"
                           "00ffff00fefefefefdfdfdfd12345678"
                           "04f5fffffe4abc"
                           "04b0"
                           "000000000000c1c2"),
                   simulation.now());
    ASSERT_EQ(server.events.datagrams.size(), 3U);
    const auto *const reply = "08"
                              "00ffff00fefefefefdfdfdfd12345678"
                              "0000000000005e5e"
                              "04f5fffffdc350"
                              "04b0"
                              "00";
    EXPECT_EQ(toHex(server.events.datagrams[0]), reply);
    EXPECT_EQ(toHex(server.events.datagrams[1]), reply);
    EXPECT_EQ(toHex(server.events.datagrams[2]), "12"
                                                 "00ffff00fefefefefdfdfdfd12345678"
                                                 "0000000000005e5e");
}

TEST(Connection, FullServerRefusesWithNoFreeIncomingConnections)
{
    Handshake handshake;
    handshake.server.accepting = false;
    ASSERT_TRUE(handshake.simulation.runUntil([&] { return !handshake.client_events.disconnected.empty(); }, 10s));
    EXPECT_EQ(handshake.client_events.disconnected,
              std::vector<DisconnectReason>{DisconnectReason::NoFreeIncomingConnections});
}

TEST(Connection, IncompatibleProtocolEndsTheAttempt)
{
    Handshake handshake;
    handshake.client.handleDatagram(fromHex("190a"
                                            "00ffff00fefefefefdfdfdfd12345678"
                                            "0000000000005e5e"),
                                    handshake.simulation.now());
    EXPECT_EQ(handshake.client_events.disconnected,
              std::vector<DisconnectReason>{DisconnectReason::IncompatibleProtocolVersion});
    EXPECT_FALSE(handshake.client.nextTimeout());
}

TEST(Connection, WrongPasswordIsRejectedOnBothSides)
{
    Handshake handshake{{.guid = 0xC1C1, .password = Unpooled::copiedBuffer(fromHex("736563726574"))}};
    ASSERT_TRUE(handshake.simulation.runUntil(
        [&] { return !handshake.client_events.disconnected.empty() && !handshake.server.events.disconnected.empty(); },
        15s));
    EXPECT_EQ(handshake.client_events.disconnected, std::vector<DisconnectReason>{DisconnectReason::InvalidPassword});
    EXPECT_EQ(handshake.server.events.disconnected, std::vector<DisconnectReason>{DisconnectReason::Rejected});
    EXPECT_EQ(handshake.client_events.connected + handshake.server.events.connected, 0);
}

TEST(Connection, CorrectPasswordIsAccepted)
{
    Handshake handshake{{.guid = 0xC1C1, .password = Unpooled::copiedBuffer(fromHex("736563726574"))}};
    handshake.server.configuration.password = Unpooled::copiedBuffer(fromHex("736563726574"));
    ASSERT_TRUE(handshake.connect());
}

TEST(Connection, UnverifiedSenderSendingAnythingButARequestIsClosed)
{
    Simulation simulation;
    Server server{simulation};
    server.receive(fromHex("07"
                           "00ffff00fefefefefdfdfdfd12345678"
                           "04f5fffffe4abc"
                           "0578"
                           "000000000000c1c1"),
                   simulation.now());
    server.receive(fromHex("84000000"
                           "400010"
                           "000000"
                           "fe01"),
                   simulation.now());
    EXPECT_EQ(server.events.disconnected, std::vector<DisconnectReason>{DisconnectReason::Rejected});
    EXPECT_TRUE(server.connection->isClosed());
}

TEST(Connection, UnverifiedSenderCannotParkMoreThanTheHandshakeBudget)
{
    Simulation simulation;
    Server server{simulation};
    server.receive(fromHex("07"
                           "00ffff00fefefefefdfdfdfd12345678"
                           "04f5fffffe4abc"
                           "0578"
                           "000000000000c1c1"),
                   simulation.now());
    const std::string fragment(2 * 1000, 'a');
    for (std::uint32_t id = 0; id < 80 && !server.connection->isClosed(); ++id) {
        const auto number = test::toHex(std::array{std::byte(id), std::byte{0}, std::byte{0}});
        const auto split_id = test::toHex(std::array{std::byte{0}, std::byte(id)});
        server.receive(fromHex("84" + number +
                               "101f40"
                               "00000002" +
                               split_id + "00000000" + fragment),
                       simulation.now());
    }
    EXPECT_TRUE(server.connection->isClosed());
    EXPECT_EQ(server.events.notifications.back(), "held split and ordered packets exceed the maximum held bytes");
}

TEST(Connection, HalfOpenConnectionIsReapedAfterTenSeconds)
{
    Simulation simulation;
    Server server{simulation};
    simulation.attach(1, {.receive = [&](std::span<const std::byte> datagram,
                                         TimePoint now) { server.receive(Unpooled::wrappedBuffer(datagram), now); },
                          .next_timeout = [&] { return server.connection->nextTimeout(); },
                          .timeout = [&](TimePoint now) { server.connection->handleTimeout(now); }});
    server.receive(fromHex("07"
                           "00ffff00fefefefefdfdfdfd12345678"
                           "04f5fffffe4abc"
                           "0578"
                           "000000000000c1c1"),
                   simulation.now());
    const auto start = simulation.now();
    ASSERT_TRUE(simulation.runUntil([&] { return !server.events.disconnected.empty(); }, 20s));
    EXPECT_EQ(server.events.disconnected, std::vector<DisconnectReason>{DisconnectReason::HandshakeTimedOut});
    EXPECT_GT(simulation.now() - start, 10s);
    EXPECT_LT(simulation.now() - start, 10s + 5ms);
    EXPECT_EQ(server.events.connected, 0);
}

TEST(Connection, IdleConnectionKeepsItselfAliveWithReliablePings)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    handshake.simulation.runUntil([] { return false; }, 1s);
    const auto before = handshake.client_events.datagrams.size();
    handshake.simulation.runUntil([] { return false; }, 60s);
    EXPECT_TRUE(handshake.client_events.disconnected.empty());
    EXPECT_TRUE(handshake.server.events.disconnected.empty());
    EXPECT_GT(handshake.client_events.datagrams.size(), before + 10);
    EXPECT_LT(handshake.client_events.datagrams.size(), before + 40);
}

TEST(Connection, SilentPeerIsLostAfterTheTimeout)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    handshake.simulation.setConditions(1, {.loss = 1.0});
    const auto start = handshake.simulation.now();
    ASSERT_TRUE(handshake.simulation.runUntil([&] { return !handshake.client_events.disconnected.empty(); }, 30s));
    EXPECT_EQ(handshake.client_events.disconnected, std::vector<DisconnectReason>{DisconnectReason::ConnectionLost});
    EXPECT_GE(handshake.simulation.now() - start, 10s);
    EXPECT_LE(handshake.simulation.now() - start, 16s);
}

TEST(Connection, GracefulCloseReachesThePeer)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    handshake.client.close(true, handshake.simulation.now());
    EXPECT_FALSE(handshake.client.send(Unpooled::copiedBuffer(payload(10)), PacketPriority::High,
                                       PacketReliability::Reliable, 0, handshake.simulation.now()));
    ASSERT_TRUE(handshake.simulation.runUntil(
        [&] { return !handshake.client_events.disconnected.empty() && !handshake.server.events.disconnected.empty(); },
        5s));
    EXPECT_EQ(handshake.client_events.disconnected, std::vector<DisconnectReason>{DisconnectReason::ClosedLocally});
    EXPECT_EQ(handshake.server.events.disconnected, std::vector<DisconnectReason>{DisconnectReason::ClosedByPeer});
}

TEST(Connection, ClosingTwiceWhileDeliveringStillReachesThePeer)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    handshake.server.events.on_message = [&] {
        handshake.server.connection->close(true, handshake.simulation.now());
        handshake.server.connection->close(true, handshake.simulation.now());
    };

    ASSERT_TRUE(handshake.client.send(Unpooled::copiedBuffer(payload(10)), PacketPriority::High,
                                      PacketReliability::Reliable, 0, handshake.simulation.now()));

    ASSERT_TRUE(handshake.simulation.runUntil([&] { return !handshake.client_events.disconnected.empty(); }, 5s));
    EXPECT_EQ(handshake.client_events.disconnected, std::vector<DisconnectReason>{DisconnectReason::ClosedByPeer});
}

TEST(Connection, ClosingConnectionEndsEvenIfThePeerKeepsSendingWithoutAcking)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    handshake.simulation.setConditions(0, {.loss = 1.0});
    handshake.server.connection->close(true, handshake.simulation.now());
    const auto start = handshake.simulation.now();
    for (std::uint32_t i = 0; i < 30 && handshake.server.events.disconnected.empty(); ++i) {
        handshake.simulation.runUntil([&] { return !handshake.server.events.disconnected.empty(); }, 5s);
        const auto number = toHex(std::array{std::byte(0x60 + i), std::byte{0}, std::byte{0}});
        handshake.server.connection->handleDatagram(fromHex("84" + number +
                                                            "000008"
                                                            "fe"),
                                                    handshake.simulation.now());
    }
    EXPECT_EQ(handshake.server.events.disconnected, std::vector<DisconnectReason>{DisconnectReason::ClosedLocally});
    EXPECT_LE(handshake.simulation.now() - start, 13s);
}

TEST(Connection, CloseWithoutNotificationIsImmediate)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    handshake.server.connection->close(false, handshake.simulation.now());
    EXPECT_EQ(handshake.server.events.disconnected, std::vector<DisconnectReason>{DisconnectReason::ClosedLocally});
    EXPECT_FALSE(handshake.server.connection->nextTimeout());
}

TEST(Connection, ClosingDuringTheOpenConnectionPhaseStopsProbing)
{
    Handshake handshake;
    handshake.simulation.setConditions(0, {.loss = 1.0});
    handshake.simulation.runUntil([] { return false; }, 700ms);
    handshake.client.close(true, handshake.simulation.now());
    EXPECT_EQ(handshake.client_events.disconnected, std::vector<DisconnectReason>{DisconnectReason::ClosedLocally});
    EXPECT_FALSE(handshake.client.nextTimeout());
}

TEST(Connection, ConnectedPingIsAnsweredWithAPong)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    handshake.simulation.runUntil([] { return false; }, 100ms);
    const auto before = handshake.server.events.datagrams.size();
    handshake.server.connection->handleDatagram(fromHex("84"
                                                        "100000"
                                                        "000048"
                                                        "00"
                                                        "0000000000001234"),
                                                handshake.simulation.now());
    ASSERT_GT(handshake.server.events.datagrams.size(), before);
    const auto &reply = handshake.server.events.datagrams.back();
    EXPECT_EQ(toHex(std::span{reply}.subspan(4, 3 + 9)), "000088"
                                                         "03"
                                                         "0000000000001234");
    EXPECT_GT(handshake.client.getLastPing().value_or(std::chrono::milliseconds{-1}).count(), -1);
}

TEST(Connection, RepeatedNewIncomingConnectionAnnouncesOnce)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    std::string incoming = "13"
                           "04f5fffffe4abc";
    for (int i = 0; i < 20; ++i) {
        incoming += "04ffffffff0000";
    }
    incoming += "00000000000000010000000000000002";
    for (std::uint32_t i = 0; i < 3; ++i) {
        const auto length = test::toHex(
            std::array{std::byte((incoming.size() / 2 * 8) >> 8U), std::byte((incoming.size() / 2 * 8) & 0xFFU)});
        handshake.server.connection->handleDatagram(
            fromHex("84" + test::toHex(std::array{std::byte(0x20 + i), std::byte{0}, std::byte{0}}) + "00" + length +
                    incoming),
            handshake.simulation.now());
    }
    EXPECT_EQ(handshake.server.events.connected, 1);
    EXPECT_TRUE(handshake.server.connection->isConnected());
}

TEST(Connection, SecondConnectionRequestIsAcceptedAgain)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    handshake.simulation.runUntil([] { return false; }, 100ms);
    const auto before = handshake.server.events.datagrams.size();
    handshake.server.connection->handleDatagram(fromHex("84"
                                                        "300000"
                                                        "40"
                                                        "0090"
                                                        "100000"
                                                        "09"
                                                        "000000000000c1c1"
                                                        "000000000000004d"
                                                        "00"),
                                                handshake.simulation.now());
    ASSERT_GT(handshake.server.events.datagrams.size(), before);
    const auto reply = toHex(handshake.server.events.datagrams.back());
    EXPECT_NE(reply.find("1004f5fffffdc350"), std::string::npos);
    EXPECT_NE(reply.find("000000000000004d"), std::string::npos);
}

TEST(Connection, DetectLostConnectionsIsAcknowledgedAndIgnored)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    const auto before = handshake.server.events.datagrams.size();
    const auto unreliable_detect_lost_connections = fromHex("84500000"
                                                            "000008"
                                                            "04");
    handshake.server.connection->handleDatagram(unreliable_detect_lost_connections, handshake.simulation.now());
    handshake.simulation.runUntil([] { return false; }, 50ms);
    EXPECT_TRUE(handshake.server.connection->isConnected());
    EXPECT_TRUE(handshake.server.events.messages.empty());
    EXPECT_TRUE(handshake.server.events.disconnected.empty());
    EXPECT_GT(handshake.server.events.datagrams.size(), before);
}

TEST(Connection, RepeatedConnectionRequestsQueueOneAnswerAtATime)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    handshake.simulation.runUntil([] { return false; }, 100ms);
    handshake.simulation.setConditions(1, {.loss = 1.0});
    for (std::uint32_t i = 0; i < 50; ++i) {
        const auto number = toHex(std::array{std::byte(0x30 + i), std::byte{0}, std::byte{0}});
        const auto reliable = toHex(std::array{std::byte(0x10 + i), std::byte{0}, std::byte{0}});
        handshake.server.connection->handleDatagram(fromHex("84" + number + "400090" + reliable +
                                                            "09"
                                                            "000000000000c1c1"
                                                            "000000000000004d"
                                                            "00"),
                                                    handshake.simulation.now());
    }
    std::size_t answers = 0;
    for (const auto &datagram : handshake.server.events.datagrams) {
        answers += toHex(datagram).contains("1004f5fffffdc350") ? 1 : 0;
    }
    EXPECT_EQ(answers, 2U);
}

TEST(Connection, UserMessagesReachTheListenerAndControlMessagesDoNot)
{
    Handshake handshake;
    ASSERT_TRUE(handshake.connect());
    handshake.server.connection->handleDatagram(fromHex("84"
                                                        "400000"
                                                        "000010"
                                                        "fe01"
                                                        "000010"
                                                        "1a01"
                                                        "000008"
                                                        "1b"),
                                                handshake.simulation.now());
    ASSERT_EQ(handshake.server.events.messages.size(), 2U);
    EXPECT_EQ(toHex(handshake.server.events.messages[0]), "fe01");
    EXPECT_EQ(toHex(handshake.server.events.messages[1]), "1b");
}

}  // namespace
}  // namespace endgate::raknet
