#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <openssl/evp.h>

#include "endgate/network/nethernet/candidates.h"
#include "endgate/network/nethernet/manager.h"
#include "endgate/network/nethernet/signaling.h"
#include "endgate/network/ping.h"

namespace {

using endgate::ByteBuf;
using endgate::nethernet::ESendType;
using endgate::nethernet::ESessionError;
using endgate::nethernet::NetworkSessionManager;
using Clock = std::chrono::steady_clock;

class Echo : public NetworkSessionManager::Listener, public endgate::nethernet::JoinHandler {
public:
    Echo(asio::io_context &io_context, const asio::ip::address &address)
        : io_context_(io_context), socket_(io_context, {address, 0}), timer_(io_context),
          manager_(*this, {.certificate = endgate::rtc::Certificate::generate(),
                           .identity = std::make_shared<endgate::nethernet::ServerIdentityAssertionGenerator>(
                               endgate::nethernet::PrivateKey{EVP_EC_gen("P-384"), EVP_PKEY_free},
                               endgate::nethernet::ServerIdentityAssertionGenerator::Config{}),
                           .session = {.max_packet_size = 8 * 1024 * 1024}})
    {
        socket_.non_blocking(true);
        signaling_ = endgate::nethernet::HttpSignalingServer::create(io_context, *this, {}, nullptr);
        signaling_->listen({address, socket_.local_endpoint().port()});
        receive();
    }

    [[nodiscard]] std::uint16_t port() const { return socket_.local_endpoint().port(); }

    void sendDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) override
    {
        asio::error_code ignored;
        socket_.send_to(asio::buffer(datagram.data(), datagram.size()), remote, 0, ignored);
    }

    void onSessionOpen(NetworkSessionManager::SessionId session, const std::string &network_id) override
    {
        std::cout << "open " << session << " " << network_id << std::endl;
    }

    void onPacket(NetworkSessionManager::SessionId session, ByteBuf packet) override
    {
        manager_.send(session, packet, ESendType::Reliable, Clock::now());
    }

    void onSessionClosed(NetworkSessionManager::SessionId session, ESessionError reason) override
    {
        std::cout << "closed " << session << " " << static_cast<std::uint32_t>(reason) << std::endl;
    }

    endgate::ServerPing getServerPing(const endgate::nethernet::PingRequest &request) override
    {
        return {endgate::ServerPing::Version{request.protocol.value_or(2193), "1.26.52"},
                endgate::ServerPing::Players{0, 10}, "Endgate Echo", "echo", endgate::GameType::Survival};
    }

    void onJoin(endgate::nethernet::JoinRequest request, endgate::nethernet::JoinResponse response) override
    {
        std::vector<asio::ip::address> reached;
        asio::error_code error;
        if (const auto literal = asio::ip::make_address(request.host, error); !error) {
            reached.push_back(literal);
        }
        else {
            asio::ip::udp::resolver resolver(io_context_);
            for (const auto &entry : resolver.resolve(request.host, "", error)) {
                reached.push_back(entry.endpoint().address());
            }
        }
        const std::vector interfaces{socket_.local_endpoint().address()};
        const auto candidates =
            endgate::nethernet::gatherCandidates(reached, request.port.value_or(port()), interfaces, port());
        const auto answer = manager_.join(request, candidates, Clock::now(), std::chrono::system_clock::now());
        if (answer) {
            response.answer(*answer);
        }
        else {
            response.reject(static_cast<std::uint32_t>(answer.error()));
        }
        rearm();
    }

private:
    void receive()
    {
        socket_.async_receive_from(
            asio::buffer(buffer_), sender_, [this](const asio::error_code &error, std::size_t size) {
                if (error == asio::error::operation_aborted) {
                    return;
                }
                if (!error) {
                    manager_.handleDatagram(Clock::now(), sender_, std::span{buffer_}.first(size));
                    rearm();
                }
                receive();
            });
    }

    void rearm()
    {
        const auto deadline = manager_.nextTimeout();
        timer_.expires_at(deadline.value_or(Clock::now() + std::chrono::hours{1}));
        timer_.async_wait([this](const asio::error_code &error) {
            if (!error) {
                manager_.handleTimeout(Clock::now());
                rearm();
            }
        });
    }

    asio::io_context &io_context_;
    asio::ip::udp::socket socket_;
    asio::steady_timer timer_;
    NetworkSessionManager manager_;
    std::shared_ptr<endgate::nethernet::HttpSignalingServer> signaling_;
    std::array<std::byte, 65536> buffer_{};
    asio::ip::udp::endpoint sender_;
};

}  // namespace

int main(int argc, char **argv)
{
    asio::io_context io_context;
    Echo echo(io_context, asio::ip::make_address(argc > 1 ? argv[1] : "127.0.0.1"));
    std::cout << "listening " << echo.port() << std::endl;
    io_context.run();
}
