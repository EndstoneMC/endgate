#include <algorithm>
#include <array>
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
#include "endgate/network/rtc/certificate.h"
#include "endgate/network/rtc/description.h"
#include "endgate/network/rtc/stun.h"

namespace endgate::nethernet {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::vector<std::byte> bytesOf(ByteBufView buf)
{
    const auto bytes = buf.nioBuffer();
    return {bytes.begin(), bytes.end()};
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
    void sendDatagram(const asio::ip::udp::endpoint &remote, ByteBufView datagram) override
    {
        outgoing.emplace_back(remote, bytesOf(datagram));
    }
    void onRouteAdded(const asio::ip::udp::endpoint &remote) override { routes.push_back(remote); }
    void onRouteRemoved(const asio::ip::udp::endpoint &remote) override { std::erase(routes, remote); }
    void onOpen() override { ++opened; }
    void onPacket(ByteBufView packet) override { packets.push_back(bytesOf(packet)); }
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

    void drive(Clock::time_point time)
    {
        now = time;
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
                dtls.send(Unpooled::wrappedBuffer(packet));
            }
        }
    }

    rtc::DtlsTransport dtls;
    rtc::SctpTransport sctp;
    PacketQueue queue;
    Clock::time_point now;
    bool sctp_started{false};
    std::optional<std::uint16_t> reliable;
    std::optional<std::uint16_t> unreliable;
    std::vector<std::vector<std::byte>> sctp_outbox;
    std::deque<std::vector<std::byte>> outgoing;
    std::vector<std::vector<std::byte>> packets;

private:
    void sendDtlsDatagram(ByteBufView datagram) override
    {
        const auto bytes = datagram.nioBuffer();
        outgoing.emplace_back(bytes.begin(), bytes.end());
    }
    void onDtlsData(ByteBufView data) override { sctp.handlePacket(data, now); }
    void onDtlsStateChanged(rtc::DtlsTransport::State) override {}

    void sendPacket(ByteBufView packet) override
    {
        const auto bytes = packet.nioBuffer();
        sctp_outbox.emplace_back(bytes.begin(), bytes.end());
    }
    void onStateChange(rtc::SctpTransport::State) override {}
    void onChannelOpen(const rtc::DataChannel &) override {}
    void onChannelMessage(const rtc::DataChannel &channel, ByteBufView message, bool) override
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
        sctp.send(type == ESendType::Reliable ? *reliable : *unreliable, std::move(fragment), true, now);
    }
    void onPacket(ByteBufView packet) override { packets.push_back(bytesOf(packet)); }
    void closeChannel(ESendType) override {}
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
        session_.handleDatagram(now_, client_address_, check.encode(local_.pwd).slice());
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
                    client_.dtls.handleDatagram(now_, Unpooled::wrappedBuffer(datagram));
                    client_.drive(now_);
                }
            }
            client_.drive(now_);
            while (!client_.outgoing.empty()) {
                auto datagram = std::move(client_.outgoing.front());
                client_.outgoing.pop_front();
                moved = true;
                session_.handleDatagram(now_, client_address_, Unpooled::wrappedBuffer(datagram));
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
    client_.queue.push(Unpooled::wrappedBuffer(hello), ESendType::Reliable);
    const auto movement = payloadOf(40, 2);
    client_.queue.push(Unpooled::wrappedBuffer(movement), ESendType::Unreliable);
    client_.drive(now_);
    pump();
    ASSERT_EQ(recorder_.packets.size(), 2U);
    EXPECT_TRUE(std::ranges::contains(recorder_.packets, hello));
    EXPECT_TRUE(std::ranges::contains(recorder_.packets, movement));

    const auto chunk = payloadOf(300000, 3);
    EXPECT_TRUE(session_.send(Unpooled::wrappedBuffer(chunk), ESendType::Reliable, now_));
    const auto update = payloadOf(60, 4);
    EXPECT_TRUE(session_.send(Unpooled::wrappedBuffer(update), ESendType::Unreliable, now_));
    pump();
    ASSERT_EQ(client_.packets.size(), 2U);
    EXPECT_TRUE(std::ranges::contains(client_.packets, chunk));
    EXPECT_TRUE(std::ranges::contains(client_.packets, update));
}

TEST_F(NetworkSessionTest, RefusesToSendBeforeItOpens)
{
    EXPECT_FALSE(session_.send(Unpooled::wrappedBuffer(payloadOf(10, 5)), ESendType::Reliable, now_));
}

TEST_F(NetworkSessionTest, IgnoresDtlsFromAnAddressWithoutAPair)
{
    const asio::ip::udp::endpoint stranger{asio::ip::make_address("198.51.100.66"), 4000};
    const std::vector<std::byte> record{std::byte{22}, std::byte{0xFE}, std::byte{0xFD}, std::byte{0}, std::byte{0}};
    session_.handleDatagram(now_, stranger, Unpooled::wrappedBuffer(record));
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
    EXPECT_FALSE(session_.send(Unpooled::wrappedBuffer(payloadOf(10, 6)), ESendType::Reliable, now_));
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
    client_.sctp.send(*client_.reliable, Unpooled::copiedBuffer(std::vector{std::byte{0}}), true, now_);
    client_.drive(now_);
    pump();
    EXPECT_EQ(recorder_.closed, std::vector{ESessionError::DataChannelClosed});
}

class DialerSessionTest : public testing::Test {
protected:
    DialerSessionTest()
        : offer_(rtc::Description::offer(dialer_creds_, dialer_certificate_->getFingerprint(),
                                         std::vector{dialer_address_})),
          answerer_(answerer_recorder_, offer_, answerer_creds_, answerer_certificate_,
                    {.max_packet_size = 8 * 1024 * 1024}, now_),
          answer_(rtc::Description::answer(offer_, answerer_creds_, answerer_certificate_->getFingerprint(),
                                           std::array{rtc::CandidateAddress{.address = answerer_address_}})),
          dialer_(dialer_recorder_, answer_, dialer_creds_, dialer_certificate_, {.max_packet_size = 8 * 1024 * 1024},
                  now_)
    {
    }

    void deliver()
    {
        while (!dialer_recorder_.outgoing.empty() || !answerer_recorder_.outgoing.empty()) {
            while (!dialer_recorder_.outgoing.empty()) {
                auto [destination, datagram] = std::move(dialer_recorder_.outgoing.front());
                dialer_recorder_.outgoing.pop_front();
                if (destination == answerer_address_) {
                    answerer_.handleDatagram(now_, dialer_address_, Unpooled::wrappedBuffer(datagram));
                }
            }
            while (!answerer_recorder_.outgoing.empty()) {
                auto [destination, datagram] = std::move(answerer_recorder_.outgoing.front());
                answerer_recorder_.outgoing.pop_front();
                if (destination == dialer_address_) {
                    dialer_.handleDatagram(now_, answerer_address_, Unpooled::wrappedBuffer(datagram));
                }
            }
        }
    }

    void pump()
    {
        for (int round = 0; round < 10000; ++round) {
            deliver();
            if ((dialer_.isOpen() || dialer_.isClosed()) && (answerer_.isOpen() || answerer_.isClosed())) {
                return;
            }
            const auto dialer_next = dialer_.nextTimeout();
            const auto answerer_next = answerer_.nextTimeout();
            std::optional<Clock::time_point> next = dialer_next;
            if (answerer_next && (!next || *answerer_next < *next)) {
                next = answerer_next;
            }
            if (!next) {
                return;
            }
            now_ = std::max(now_, *next);
            if (dialer_next && *dialer_next <= now_) {
                dialer_.handleTimeout(now_);
            }
            if (answerer_next && *answerer_next <= now_) {
                answerer_.handleTimeout(now_);
            }
        }
        FAIL() << "the two sides never went quiet";
    }

    Clock::time_point now_{Clock::now()};
    std::shared_ptr<const rtc::Certificate> dialer_certificate_{rtc::Certificate::generate()};
    std::shared_ptr<const rtc::Certificate> answerer_certificate_{rtc::Certificate::generate()};
    rtc::IceCredentials dialer_creds_{rtc::IceCredentials::generate()};
    rtc::IceCredentials answerer_creds_{rtc::IceCredentials::generate()};
    asio::ip::udp::endpoint dialer_address_{asio::ip::make_address("203.0.113.7"), 41234};
    asio::ip::udp::endpoint answerer_address_{asio::ip::make_address("198.51.100.8"), 19132};
    Recorder dialer_recorder_;
    Recorder answerer_recorder_;
    rtc::Description offer_;
    NetworkSession answerer_;
    rtc::Description answer_;
    DialerSession dialer_;
};

TEST_F(DialerSessionTest, OpensBothSidesWhenTheDialerOffersAndOpensItsChannels)
{
    pump();
    EXPECT_TRUE(dialer_.isOpen());
    EXPECT_TRUE(answerer_.isOpen());
    EXPECT_EQ(dialer_recorder_.opened, 1);
    EXPECT_EQ(answerer_recorder_.opened, 1);
    EXPECT_TRUE(dialer_recorder_.closed.empty());
    EXPECT_TRUE(answerer_recorder_.closed.empty());
    EXPECT_EQ(dialer_recorder_.routes, std::vector{answerer_address_});
}

TEST_F(DialerSessionTest, CarriesPacketsBothWaysOnBothChannels)
{
    pump();
    ASSERT_TRUE(dialer_.isOpen());
    ASSERT_TRUE(answerer_.isOpen());

    const auto reliable = payloadOf(300000, 11);
    EXPECT_TRUE(dialer_.send(Unpooled::wrappedBuffer(reliable), ESendType::Reliable, now_));
    const auto unreliable = payloadOf(40, 12);
    EXPECT_TRUE(dialer_.send(Unpooled::wrappedBuffer(unreliable), ESendType::Unreliable, now_));
    pump();
    ASSERT_EQ(answerer_recorder_.packets.size(), 2U);
    EXPECT_TRUE(std::ranges::contains(answerer_recorder_.packets, reliable));
    EXPECT_TRUE(std::ranges::contains(answerer_recorder_.packets, unreliable));

    const auto back = payloadOf(1200, 13);
    EXPECT_TRUE(answerer_.send(Unpooled::wrappedBuffer(back), ESendType::Reliable, now_));
    pump();
    ASSERT_EQ(dialer_recorder_.packets.size(), 1U);
    EXPECT_EQ(dialer_recorder_.packets.front(), back);
}

TEST_F(DialerSessionTest, ReportsALocalCloseOnceAndStopsSending)
{
    pump();
    ASSERT_TRUE(dialer_.isOpen());
    dialer_.close(now_);
    dialer_.close(now_);
    EXPECT_EQ(dialer_recorder_.closed, std::vector{ESessionError::None});
    EXPECT_FALSE(dialer_.isOpen());
    EXPECT_FALSE(dialer_.send(Unpooled::wrappedBuffer(payloadOf(10, 14)), ESendType::Reliable, now_));
    EXPECT_TRUE(dialer_recorder_.routes.empty());
}

TEST_F(DialerSessionTest, FailsWithIceWhenNothingAnswers)
{
    Recorder lonely_recorder;
    DialerSession lonely(lonely_recorder, answer_, rtc::IceCredentials::generate(), dialer_certificate_,
                         {.max_packet_size = 8 * 1024 * 1024}, now_);
    for (auto deadline = lonely.nextTimeout(); deadline && lonely_recorder.closed.empty();
         deadline = lonely.nextTimeout()) {
        now_ = *deadline;
        lonely_recorder.outgoing.clear();
        lonely.handleTimeout(now_);
    }
    EXPECT_EQ(lonely_recorder.closed, std::vector{ESessionError::ICE});
}

}  // namespace
}  // namespace endgate::nethernet
