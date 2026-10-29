#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/rtc/certificate.h"
#include "endgate/network/rtc/dtls.h"

namespace endgate::rtc {
namespace {

using namespace std::chrono_literals;
using State = DtlsTransport::State;

class Peer : public DtlsTransport::Listener {
public:
    void sendDtlsDatagram(ByteBufView datagram) override
    {
        const auto bytes = datagram.nioBuffer();
        outbox.emplace_back(bytes.begin(), bytes.end());
    }
    void onDtlsData(ByteBufView data) override
    {
        const auto bytes = data.nioBuffer();
        received.emplace_back(bytes.begin(), bytes.end());
    }
    void onDtlsStateChanged(State state) override { states.push_back(state); }

    std::vector<std::vector<std::byte>> outbox;
    std::vector<std::vector<std::byte>> received;
    std::vector<State> states;
    std::size_t to_drop{0};
    std::size_t sent{0};
};

class DtlsTransportTest : public testing::Test {
protected:
    static DtlsTransport::Config config()
    {
        return {.mtu = 1200, .initial_retransmission_timeout = 50ms, .handshake_timeout = 3s};
    }

    void pump()
    {
        while (!client_peer_.outbox.empty() || !server_peer_.outbox.empty()) {
            for (auto [from, to] : {std::pair{&client_peer_, &server_}, std::pair{&server_peer_, &client_}}) {
                auto datagrams = std::exchange(from->outbox, {});
                for (const auto &datagram : datagrams) {
                    ++from->sent;
                    EXPECT_LE(datagram.size(), config().mtu);
                    if (from->to_drop > 0) {
                        --from->to_drop;
                        continue;
                    }
                    to->handleDatagram(std::chrono::steady_clock::now(), Unpooled::wrappedBuffer(datagram));
                }
            }
        }
    }

    bool runUntil(auto done)
    {
        const auto give_up = std::chrono::steady_clock::now() + 5s;
        pump();
        while (!done()) {
            if (std::chrono::steady_clock::now() > give_up) {
                return false;
            }
            std::this_thread::sleep_for(5ms);
            client_.handleTimeout(std::chrono::steady_clock::now());
            server_.handleTimeout(std::chrono::steady_clock::now());
            pump();
        }
        return true;
    }

    void handshake()
    {
        client_.start(std::chrono::steady_clock::now());
        server_.start(std::chrono::steady_clock::now());
        ASSERT_TRUE(
            runUntil([&] { return client_.getState() == State::Connected && server_.getState() == State::Connected; }));
    }

    std::shared_ptr<const Certificate> client_certificate_{Certificate::generate()};
    std::shared_ptr<const Certificate> server_certificate_{Certificate::generate()};
    Peer client_peer_;
    Peer server_peer_;
    DtlsTransport client_{client_peer_,
                          client_certificate_,
                          DtlsTransport::Role::Client,
                          {server_certificate_->getFingerprint()},
                          config()};
    DtlsTransport server_{server_peer_,
                          server_certificate_,
                          DtlsTransport::Role::Server,
                          {client_certificate_->getFingerprint()},
                          config()};
};

std::vector<std::byte> payloadOf(std::size_t size, std::uint8_t seed)
{
    std::vector<std::byte> payload(size);
    for (std::size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<std::byte>(seed + i);
    }
    return payload;
}

TEST_F(DtlsTransportTest, CompletesAHandshake)
{
    handshake();
    EXPECT_EQ(client_peer_.states, (std::vector{State::Connecting, State::Connected}));
    EXPECT_EQ(server_peer_.states, (std::vector{State::Connecting, State::Connected}));
}

TEST_F(DtlsTransportTest, CarriesDataBothWaysWithItsBoundaries)
{
    handshake();
    const auto small = payloadOf(12, 1);
    const auto large = payloadOf(1100, 7);
    EXPECT_TRUE(client_.send(Unpooled::wrappedBuffer(small)));
    EXPECT_TRUE(client_.send(Unpooled::wrappedBuffer(large)));
    EXPECT_TRUE(server_.send(Unpooled::wrappedBuffer(large)));
    pump();
    EXPECT_EQ(server_peer_.received, (std::vector<std::vector<std::byte>>{small, large}));
    EXPECT_EQ(client_peer_.received, (std::vector<std::vector<std::byte>>{large}));
}

TEST_F(DtlsTransportTest, SendsEveryRecordAsWholeRecordsInOneDatagram)
{
    client_.start(std::chrono::steady_clock::now());
    server_.start(std::chrono::steady_clock::now());
    std::vector<std::vector<std::byte>> seen;
    while (!client_peer_.outbox.empty() || !server_peer_.outbox.empty()) {
        seen.insert(seen.end(), client_peer_.outbox.begin(), client_peer_.outbox.end());
        seen.insert(seen.end(), server_peer_.outbox.begin(), server_peer_.outbox.end());
        pump();
    }
    ASSERT_FALSE(seen.empty());
    for (const auto &datagram : seen) {
        std::size_t offset = 0;
        while (offset + 13 <= datagram.size()) {
            const auto length = (std::to_integer<std::size_t>(datagram[offset + 11]) << 8U) |
                                std::to_integer<std::size_t>(datagram[offset + 12]);
            const auto content_type = std::to_integer<int>(datagram[offset]);
            EXPECT_TRUE(content_type >= 20 && content_type <= 23);
            offset += 13 + length;
        }
        EXPECT_EQ(offset, datagram.size());
    }
}

TEST_F(DtlsTransportTest, RetransmitsAfterLosingTheFirstFlights)
{
    client_peer_.to_drop = 1;
    server_peer_.to_drop = 2;
    handshake();
    EXPECT_GT(client_peer_.sent, 2U);

    EXPECT_TRUE(client_.send(Unpooled::wrappedBuffer(payloadOf(64, 3))));
    pump();
    EXPECT_EQ(server_peer_.received.size(), 1U);
}

TEST_F(DtlsTransportTest, FailsOnAFingerprintMismatch)
{
    const auto impostor = Certificate::generate();
    Peer peer;
    DtlsTransport client{
        peer, impostor, DtlsTransport::Role::Client, {server_certificate_->getFingerprint()}, config()};
    client.start(std::chrono::steady_clock::now());
    server_.start(std::chrono::steady_clock::now());
    for (int round = 0; round < 8; ++round) {
        for (const auto &datagram : std::exchange(peer.outbox, {})) {
            server_.handleDatagram(std::chrono::steady_clock::now(), Unpooled::wrappedBuffer(datagram));
        }
        for (const auto &datagram : std::exchange(server_peer_.outbox, {})) {
            client.handleDatagram(std::chrono::steady_clock::now(), Unpooled::wrappedBuffer(datagram));
        }
    }
    EXPECT_EQ(server_.getState(), State::Failed);
    EXPECT_EQ(client.getState(), State::Failed);
    EXPECT_FALSE(server_.send(Unpooled::wrappedBuffer(payloadOf(4, 0))));
}

TEST_F(DtlsTransportTest, FailsWhenTheServerFingerprintIsWrong)
{
    Peer peer;
    DtlsTransport client{
        peer, client_certificate_, DtlsTransport::Role::Client, {Certificate::generate()->getFingerprint()}, config()};
    client.start(std::chrono::steady_clock::now());
    server_.start(std::chrono::steady_clock::now());
    for (int round = 0; round < 8; ++round) {
        for (const auto &datagram : std::exchange(peer.outbox, {})) {
            server_.handleDatagram(std::chrono::steady_clock::now(), Unpooled::wrappedBuffer(datagram));
        }
        for (const auto &datagram : std::exchange(server_peer_.outbox, {})) {
            client.handleDatagram(std::chrono::steady_clock::now(), Unpooled::wrappedBuffer(datagram));
        }
    }
    EXPECT_EQ(client.getState(), State::Failed);
    EXPECT_NE(server_.getState(), State::Connected);
}

TEST_F(DtlsTransportTest, ClosesOnCloseNotify)
{
    handshake();
    client_.close();
    EXPECT_EQ(client_.getState(), State::Closed);
    pump();
    EXPECT_EQ(server_.getState(), State::Closed);
    EXPECT_FALSE(client_.nextTimeout());
    EXPECT_FALSE(server_.nextTimeout());
}

TEST_F(DtlsTransportTest, FailsWhenTheHandshakeNeverCompletes)
{
    const auto now = std::chrono::steady_clock::now();
    client_.start(now);
    ASSERT_TRUE(client_.nextTimeout());
    EXPECT_LE(*client_.nextTimeout(), now + 3s);
    client_.handleTimeout(now + 3s);
    EXPECT_EQ(client_.getState(), State::Failed);
    EXPECT_FALSE(client_.nextTimeout());
}

TEST_F(DtlsTransportTest, IgnoresGarbage)
{
    client_.start(std::chrono::steady_clock::now());
    const auto garbage = payloadOf(40, 0x16);
    server_.handleDatagram(std::chrono::steady_clock::now(), Unpooled::wrappedBuffer(garbage));
    handshake();
    client_.handleDatagram(std::chrono::steady_clock::now(), Unpooled::wrappedBuffer(garbage));
    server_.handleDatagram(std::chrono::steady_clock::now(), Unpooled::wrappedBuffer(payloadOf(3, 23)));
    EXPECT_EQ(client_.getState(), State::Connected);
    EXPECT_EQ(server_.getState(), State::Connected);
    EXPECT_TRUE(server_.send(Unpooled::wrappedBuffer(payloadOf(5, 5))));
    pump();
    EXPECT_EQ(client_peer_.received.size(), 1U);
}

}  // namespace
}  // namespace endgate::rtc
