#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <openssl/evp.h>

#include "endgate/network/buffer.h"
#include "endgate/network/nethernet/client.h"
#include "endgate/network/nethernet/identity.h"
#include "endgate/network/nethernet/session.h"
#include "endgate/network/rtc/certificate.h"
#include "endgate/network/rtc/description.h"

namespace {

using endgate::ByteBuf;
using endgate::nethernet::DialerSession;
using endgate::nethernet::ESendType;
using endgate::nethernet::ESessionError;
using endgate::nethernet::SignalingClient;
using Clock = std::chrono::steady_clock;

std::vector<std::byte> payloadOf(std::size_t size)
{
    std::vector<std::byte> payload(size);
    for (std::size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<std::byte>((i * 37 + 7) & 0xFF);
    }
    return payload;
}

class Dialer : public DialerSession::Listener {
public:
    Dialer(asio::io_context &io_context, std::string host, std::uint16_t port, std::string network_id)
        : io_context_(io_context), host_(std::move(host)), port_(port), network_id_(std::move(network_id)),
          socket_(io_context, {asio::ip::udp::v4(), 0}), timer_(io_context), deadline_(io_context)
    {
        socket_.non_blocking(true);
    }

    [[nodiscard]] bool succeeded() const { return succeeded_; }

    asio::awaitable<void> run()
    {
        deadline_.expires_after(std::chrono::seconds{20});
        deadline_.async_wait([this](const asio::error_code &error) {
            if (!error) {
                std::cout << "timeout\n";
                stop();
            }
        });

        auto offer = endgate::rtc::Description::offer(credentials_, certificate_->getFingerprint(),
                                                      std::array{socket_.local_endpoint()})
                         .generate(0x1122334455667788ULL);
        if (const auto identity =
                endgate::nethernet::IdentityAssertion::create(*generator_, offer, std::chrono::system_clock::now())) {
            offer = identity->injectIntoSdp(offer);
        }

        const auto answer = co_await SignalingClient::requestAnswer(
            io_context_, host_, port_, network_id_, offer,
            {.connect_timeout = std::chrono::seconds{5}, .request_timeout = std::chrono::seconds{10}});
        if (!answer) {
            std::cout << "error " << static_cast<std::uint32_t>(answer.error()) << '\n';
            stop();
            co_return;
        }

        const auto description = endgate::rtc::Description::parse(answer->sdp);
        if (!description) {
            std::cout << "parse-failed\n";
            stop();
            co_return;
        }
        if (answer->identity) {
            std::cout << "identity verified=" << (answer->identity->verified ? 1 : 0)
                      << " domain=" << answer->identity->idp_domain << '\n';
        }
        session_.emplace(*this, *description, credentials_, certificate_,
                         DialerSession::Config{.max_packet_size = 8 * 1024 * 1024}, Clock::now());
        receive();
        rearm();
    }

    void sendDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) override
    {
        asio::error_code ignored;
        socket_.send_to(asio::buffer(datagram.data(), datagram.size()), remote, 0, ignored);
    }
    void onRouteAdded(const asio::ip::udp::endpoint &) override {}
    void onRouteRemoved(const asio::ip::udp::endpoint &) override {}

    void onOpen() override
    {
        std::cout << "open\n";
        session_->send(ByteBuf::copyBuffer(payload_), ESendType::Reliable, Clock::now());
        rearm();
    }

    void onPacket(ByteBuf packet) override
    {
        std::vector<std::byte> bytes(packet.length());
        packet.copyTo(bytes);
        if (bytes == payload_) {
            std::cout << "echo-ok\n";
            succeeded_ = true;
        }
        else {
            std::cout << "echo-mismatch\n";
        }
        stop();
    }

    void onClosed(ESessionError reason) override
    {
        std::cout << "closed " << static_cast<std::uint32_t>(reason) << '\n';
        stop();
    }

private:
    void receive()
    {
        socket_.async_receive_from(
            asio::buffer(buffer_), sender_, [this](const asio::error_code &error, std::size_t size) {
                if (error == asio::error::operation_aborted) {
                    return;
                }
                if (!error && session_) {
                    session_->handleDatagram(Clock::now(), sender_, std::span{buffer_}.first(size));
                    rearm();
                }
                receive();
            });
    }

    void rearm()
    {
        if (!session_) {
            return;
        }
        const auto deadline = session_->nextTimeout();
        timer_.expires_at(deadline.value_or(Clock::now() + std::chrono::hours{1}));
        timer_.async_wait([this](const asio::error_code &error) {
            if (!error && session_) {
                session_->handleTimeout(Clock::now());
                rearm();
            }
        });
    }

    void stop()
    {
        asio::error_code ignored;
        deadline_.cancel();
        timer_.cancel();
        std::ignore = socket_.close(ignored);
        io_context_.stop();
    }

    asio::io_context &io_context_;
    std::string host_;
    std::uint16_t port_;
    std::string network_id_;
    asio::ip::udp::socket socket_;
    asio::steady_timer timer_;
    asio::steady_timer deadline_;
    std::shared_ptr<const endgate::rtc::Certificate> certificate_{endgate::rtc::Certificate::generate()};
    std::shared_ptr<endgate::nethernet::ServerIdentityAssertionGenerator> generator_{
        std::make_shared<endgate::nethernet::ServerIdentityAssertionGenerator>(
            endgate::nethernet::PrivateKey{EVP_EC_gen("P-384"), EVP_PKEY_free},
            endgate::nethernet::ServerIdentityAssertionGenerator::Config{})};
    endgate::rtc::IceCredentials credentials_{endgate::rtc::IceCredentials::generate()};
    std::vector<std::byte> payload_{payloadOf(5000)};
    std::array<std::byte, 65536> buffer_{};
    asio::ip::udp::endpoint sender_;
    std::optional<DialerSession> session_;
    bool succeeded_{false};
};

}  // namespace

int main(int argc, char **argv)
{
    std::cout << std::unitbuf;
    if (argc < 3) {
        std::cerr << "usage: nethernet_dial <host> <port> [networkId]\n";
        return 2;
    }
    std::uint16_t port = 0;
    const std::string_view port_text(argv[2]);
    if (std::from_chars(port_text.data(), port_text.data() + port_text.size(), port).ec != std::errc{}) {
        std::cerr << "invalid port\n";
        return 2;
    }
    asio::io_context io_context;
    Dialer dialer(io_context, argv[1], port, argc > 3 ? argv[3] : "endgate-dialer");
    asio::co_spawn(io_context, dialer.run(), asio::detached);
    io_context.run();
    return dialer.succeeded() ? 0 : 1;
}
