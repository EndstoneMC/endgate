#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>
#include <asio/ip/udp.hpp>
#include <gtest/gtest.h>

#include "endgate/network/nethernet/session.h"
#include "endgate/network/rtc/stun.h"

namespace endgate::nethernet {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

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
        payload[i] = static_cast<std::byte>((i * 31 + seed) & 0xFF);
    }
    return payload;
}

class Recorder : public NetworkSession::Listener {
public:
    void sendDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) override
    {
        outgoing.emplace_back(remote, std::vector<std::byte>(datagram.begin(), datagram.end()));
    }
    void onRouteAdded(const asio::ip::udp::endpoint &remote) override { routes.push_back(remote); }
    void onRouteRemoved(const asio::ip::udp::endpoint &remote) override { std::erase(routes, remote); }
    void onOpen() override { ++opened; }
    void onPacket(ByteBuf packet) override { packets.push_back(bytesOf(packet)); }
    void onClosed(ESessionError reason) override { closed.push_back(reason); }

    std::deque<std::pair<asio::ip::udp::endpoint, std::vector<std::byte>>> outgoing;
    std::vector<asio::ip::udp::endpoint> routes;
    int opened{0};
    std::vector<std::vector<std::byte>> packets;
    std::vector<ESessionError> closed;
};

class Client : rtc::DtlsTransport::Listener, rtc::SctpTransport::Listener, PacketQueue::Listener {
public:
    explicit Client(std::shared_ptr<const rtc::Certificate> certificate,
                    std::vector<rtc::CertificateFingerprint> server_fingerprints)
        : dtls(*this, std::move(certificate), rtc::DtlsTransport::Role::Server, std::move(server_fingerprints), {}),
          sctp(*this, {.dtls_client = false, .remote_max_message_size = 262144}), queue(*this, 8 * 1024 * 1024)
    {
    }

    void drive(Clock::time_point now)
    {
        now_ = now;
        if (dtls.getState() == rtc::DtlsTransport::State::Connected && !sctp_started) {
            sctp_started = true;
            sctp.start(now);
        }
        if (sctp.state() == rtc::SctpTransport::State::Connected && !reliable) {
            reliable = sctp.createDataChannel("ReliableDataChannel", "", {}, 0, now);
            unreliable =
                sctp.createDataChannel("UnreliableDataChannel", "", {.unordered = true, .max_retransmits = 0}, 0, now);
        }
        while (dtls.getState() == rtc::DtlsTransport::State::Connected && !sctp_outbox.empty()) {
            for (const auto &packet : std::exchange(sctp_outbox, {})) {
                dtls.send(packet);
            }
        }
    }

    void sendDtlsDatagram(std::span<const std::byte> datagram) override
    {
        outgoing.emplace_back(datagram.begin(), datagram.end());
    }
    void onDtlsData(std::span<const std::byte> data) override { sctp.handlePacket(data, now_); }
    void onDtlsStateChanged(rtc::DtlsTransport::State) override {}

    void sendPacket(std::span<const std::byte> packet) override
    {
        sctp_outbox.emplace_back(packet.begin(), packet.end());
    }
    void onStateChange(rtc::SctpTransport::State) override {}
    void onChannelOpen(const rtc::DataChannel &) override {}
    void onChannelMessage(const rtc::DataChannel &channel, ByteBuf message, bool) override
    {
        if (channel.stream == reliable) {
            queue.onOrderedMessage(message);
        }
        else if (channel.stream == unreliable) {
            queue.onUnorderedMessage(message);
        }
    }
    void onChannelClosed(const rtc::DataChannel &) override {}

    void sendFragment(ESendType type, ByteBuf fragment) override
    {
        sctp.send(type == ESendType::Reliable ? *reliable : *unreliable, std::move(fragment), true, now_);
    }
    void onPacket(ByteBuf packet) override { packets.push_back(bytesOf(packet)); }
    void closeChannel(ESendType) override {}

    rtc::DtlsTransport dtls;
    rtc::SctpTransport sctp;
    PacketQueue queue;
    Clock::time_point now_{};
    bool sctp_started{false};
    std::optional<std::uint16_t> reliable;
    std::optional<std::uint16_t> unreliable;
    std::vector<std::vector<std::byte>> sctp_outbox;
    std::deque<std::vector<std::byte>> outgoing;
    std::vector<std::vector<std::byte>> packets;
};

class NetworkSessionTest : public testing::Test {
protected:
    NetworkSessionTest()
        : offer_(makeOffer()),
          session_(recorder_, offer_, local_, server_certificate_, {.max_packet_size = 8 * 1024 * 1024}, now_)
    {
    }

    rtc::Description makeOffer()
    {
        rtc::Description offer;
        offer.ice_ufrag = "clnt";
        offer.ice_pwd = "abcdefghijklmnopqrstuvwx";
        offer.fingerprints = {client_certificate_->getFingerprint()};
        offer.setup = rtc::DtlsSetup::ActPass;
        offer.max_message_size = 262144;
        return offer;
    }

    void nominate()
    {
        rtc::StunMessage check;
        check.transaction_id[0] = std::byte{0x42};
        check.username = local_.ufrag + ":" + offer_.ice_ufrag;
        check.priority = 1845501695;
        check.ice_controlling = 0x1122334455667788ULL;
        check.use_candidate = true;
        session_.handleDatagram(now_, client_address_, check.encode(local_.pwd));
    }

    void pump()
    {
        for (int round = 0; round < 10000; ++round) {
            bool moved = false;
            while (!recorder_.outgoing.empty()) {
                auto [remote, datagram] = std::move(recorder_.outgoing.front());
                recorder_.outgoing.pop_front();
                moved = true;
                if (remote == client_address_ && std::to_integer<std::uint8_t>(datagram.front()) >= 20) {
                    client_.dtls.handleDatagram(now_, datagram);
                    client_.drive(now_);
                }
            }
            client_.drive(now_);
            while (!client_.outgoing.empty()) {
                auto datagram = std::move(client_.outgoing.front());
                client_.outgoing.pop_front();
                moved = true;
                session_.handleDatagram(now_, client_address_, datagram);
            }
            if (!moved) {
                return;
            }
        }
        FAIL() << "the two sides never went quiet";
    }

    void connect()
    {
        nominate();
        pump();
    }

    Clock::time_point now_{Clock::now()};
    std::shared_ptr<const rtc::Certificate> server_certificate_{rtc::Certificate::generate()};
    std::shared_ptr<const rtc::Certificate> client_certificate_{rtc::Certificate::generate()};
    rtc::IceCredentials local_{rtc::IceCredentials::generate()};
    rtc::Description offer_;
    asio::ip::udp::endpoint client_address_{asio::ip::make_address("203.0.113.7"), 51855};
    Recorder recorder_;
    Client client_{client_certificate_, {server_certificate_->getFingerprint()}};
    NetworkSession session_;
};

TEST_F(NetworkSessionTest, OpensWhenTheClientsTwoChannelsArrive)
{
    connect();
    EXPECT_EQ(recorder_.routes, std::vector{client_address_});
    EXPECT_EQ(recorder_.opened, 1);
    EXPECT_TRUE(session_.isOpen());
    EXPECT_TRUE(recorder_.closed.empty());
}

TEST_F(NetworkSessionTest, CarriesPacketsBothWaysOnBothChannels)
{
    connect();
    ASSERT_TRUE(session_.isOpen());

    const auto hello = payloadOf(100, 1);
    client_.queue.push(ByteBuf::copyBuffer(hello), ESendType::Reliable);
    const auto movement = payloadOf(40, 2);
    client_.queue.push(ByteBuf::copyBuffer(movement), ESendType::Unreliable);
    client_.drive(now_);
    pump();
    ASSERT_EQ(recorder_.packets.size(), 2U);
    EXPECT_TRUE(std::ranges::contains(recorder_.packets, hello));
    EXPECT_TRUE(std::ranges::contains(recorder_.packets, movement));

    const auto chunk = payloadOf(300000, 3);
    EXPECT_TRUE(session_.send(ByteBuf::copyBuffer(chunk), ESendType::Reliable, now_));
    const auto update = payloadOf(60, 4);
    EXPECT_TRUE(session_.send(ByteBuf::copyBuffer(update), ESendType::Unreliable, now_));
    pump();
    ASSERT_EQ(client_.packets.size(), 2U);
    EXPECT_TRUE(std::ranges::contains(client_.packets, chunk));
    EXPECT_TRUE(std::ranges::contains(client_.packets, update));
}

TEST_F(NetworkSessionTest, RefusesToSendBeforeItOpens)
{
    EXPECT_FALSE(session_.send(ByteBuf::copyBuffer(payloadOf(10, 5)), ESendType::Reliable, now_));
}

TEST_F(NetworkSessionTest, IgnoresDtlsFromAnAddressWithoutAPair)
{
    const asio::ip::udp::endpoint stranger{asio::ip::make_address("198.51.100.66"), 4000};
    const std::vector<std::byte> record{std::byte{22}, std::byte{0xFE}, std::byte{0xFD}, std::byte{0}, std::byte{0}};
    session_.handleDatagram(now_, stranger, record);
    EXPECT_TRUE(recorder_.outgoing.empty());
    connect();
    EXPECT_TRUE(session_.isOpen());
}

TEST_F(NetworkSessionTest, FailsWithIceWhenNothingIsNominated)
{
    for (auto deadline = session_.nextTimeout(); deadline && recorder_.closed.empty();
         deadline = session_.nextTimeout()) {
        now_ = *deadline;
        session_.handleTimeout(now_);
    }
    EXPECT_EQ(recorder_.closed, std::vector{ESessionError::ICE});
    EXPECT_FALSE(session_.nextTimeout());
}

TEST_F(NetworkSessionTest, ReportsALocalCloseOnce)
{
    connect();
    session_.close(now_);
    session_.close(now_);
    EXPECT_EQ(recorder_.closed, std::vector{ESessionError::None});
    EXPECT_FALSE(session_.isOpen());
    EXPECT_FALSE(session_.send(ByteBuf::copyBuffer(payloadOf(10, 6)), ESendType::Reliable, now_));
}

TEST_F(NetworkSessionTest, ClosesWhenTheClientClosesItsReliableChannel)
{
    connect();
    ASSERT_TRUE(client_.reliable);
    client_.sctp.closeDataChannel(*client_.reliable, now_);
    client_.drive(now_);
    pump();
    EXPECT_EQ(recorder_.closed, std::vector{ESessionError::DataChannelClosed});
}

TEST_F(NetworkSessionTest, ClosesOnAHeaderOnlyMessage)
{
    connect();
    client_.sctp.send(*client_.reliable, ByteBuf::copyBuffer(std::vector{std::byte{0}}), true, now_);
    client_.drive(now_);
    pump();
    EXPECT_EQ(recorder_.closed, std::vector{ESessionError::DataChannelClosed});
}

}  // namespace
}  // namespace endgate::nethernet
