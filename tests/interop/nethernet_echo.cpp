#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <openssl/evp.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/eventloop.h"
#include "endgate/network/nethernet/candidates.h"
#include "endgate/network/nethernet/identity.h"
#include "endgate/network/nethernet/session.h"
#include "endgate/network/nethernet/signaling.h"
#include "endgate/network/ping.h"
#include "endgate/network/rtc/certificate.h"
#include "endgate/network/rtc/stun.h"

namespace {

using endgate::ByteBufView;
using endgate::nethernet::ESendType;
using endgate::nethernet::ESessionError;
using endgate::nethernet::NetworkSession;
using Clock = std::chrono::steady_clock;

class Echo : public endgate::nethernet::JoinHandler {
public:
    Echo(endgate::EventLoop &event_loop, const asio::ip::address &address)
        : event_loop_(event_loop), io_context_(event_loop.getIoContext()), socket_(io_context_, {address, 0}),
          timer_(io_context_), certificate_(endgate::rtc::Certificate::generate()),
          identity_(endgate::nethernet::PrivateKey{EVP_EC_gen("P-384"), EVP_PKEY_free},
                    endgate::nethernet::ServerIdentityAssertionGenerator::Config{})
    {
        socket_.non_blocking(true);
        signaling_ = endgate::nethernet::HttpSignalingServer::create(event_loop, *this, {}, nullptr);
        signaling_->listen({address, socket_.local_endpoint().port()});
        receive();
    }

    [[nodiscard]] std::uint16_t port() const { return socket_.local_endpoint().port(); }

    endgate::ServerPing getServerPing(const endgate::nethernet::PingRequest &request) override
    {
        return {endgate::ServerPing::Version{request.protocol.value_or(2193), "1.26.52"},
                endgate::ServerPing::Players{0, 10}, "Endgate Echo", "echo", endgate::GameType::Survival};
    }

    void onJoin(endgate::nethernet::JoinRequest request, endgate::nethernet::JoinResponse response) override
    {
        const auto offer =
            endgate::rtc::Description::parse(endgate::nethernet::IdentityAssertion::stripFromSdp(request.offer));
        if (!offer) {
            response.reject(static_cast<std::uint32_t>(ESessionError::FailedToSetRemoteDescription));
            return;
        }
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

        auto local = endgate::rtc::IceCredentials::generate();
        while (ufrags_.contains(local.ufrag)) {
            local = endgate::rtc::IceCredentials::generate();
        }
        const auto id = next_id_++;
        auto sdp =
            endgate::rtc::Description::answer(*offer, local, certificate_->getFingerprint(), candidates).generate(id);
        const auto identity =
            endgate::nethernet::IdentityAssertion::create(identity_, sdp, std::chrono::system_clock::now());
        if (!identity) {
            response.reject(static_cast<std::uint32_t>(ESessionError::FailedToCreateIdentityAssertion));
            return;
        }
        const auto ufrag = local.ufrag;
        ufrags_.emplace(ufrag, id);
        sessions_.emplace(id, std::make_unique<Session>(*this, id, request.network_id, ufrag, *offer, std::move(local),
                                                        Clock::now()));
        response.answer(identity->injectIntoSdp(sdp));
        settle();
    }

private:
    class Session : public NetworkSession::Listener {
    public:
        Session(Echo &echo, std::uint64_t id, std::string network_id, std::string ufrag,
                const endgate::rtc::Description &offer, endgate::rtc::IceCredentials local, Clock::time_point now)
            : echo(echo), id(id), network_id(std::move(network_id)), ufrag(std::move(ufrag)),
              session(*this, offer, std::move(local), echo.certificate_, {.max_packet_size = 8 * 1024 * 1024}, now)
        {
        }

        void sendDatagram(const asio::ip::udp::endpoint &remote, ByteBufView datagram) override
        {
            asio::error_code ignored;
            echo.socket_.send_to(asio::buffer(datagram.nioBuffer()), remote, 0, ignored);
        }

        void onRouteAdded(const asio::ip::udp::endpoint &remote) override { echo.routes_[remote] = id; }

        void onRouteRemoved(const asio::ip::udp::endpoint &remote) override
        {
            if (const auto route = echo.routes_.find(remote); route != echo.routes_.end() && route->second == id) {
                echo.routes_.erase(route);
            }
        }

        void onOpen() override { std::cout << "open " << id << " " << network_id << '\n'; }

        void onPacket(ByteBufView packet) override { session.send(packet, ESendType::Reliable, Clock::now()); }

        void onClosed(ESessionError reason) override { closed = reason; }

        Echo &echo;
        std::uint64_t id;
        std::string network_id;
        std::string ufrag;
        std::optional<ESessionError> closed;
        NetworkSession session;
    };

    void receive()
    {
        socket_.async_receive_from(
            asio::buffer(buffer_), sender_, [this](const asio::error_code &error, std::size_t size) {
                if (error == asio::error::operation_aborted) {
                    return;
                }
                if (!error) {
                    const auto view = endgate::Unpooled::wrappedBuffer(std::span{buffer_}.first(size));
                    std::optional<std::uint64_t> owner;
                    if (const auto route = routes_.find(sender_); route != routes_.end()) {
                        owner = route->second;
                    }
                    else if (const auto ufrag = endgate::rtc::StunMessage::peekLocalUfrag(view)) {
                        if (const auto known = ufrags_.find(std::string(*ufrag)); known != ufrags_.end()) {
                            owner = known->second;
                        }
                    }
                    if (const auto entry = owner ? sessions_.find(*owner) : sessions_.end(); entry != sessions_.end()) {
                        entry->second->session.handleDatagram(Clock::now(), sender_, view);
                    }
                    settle();
                }
                receive();
            });
    }

    void settle()
    {
        for (auto entry = sessions_.begin(); entry != sessions_.end();) {
            if (!entry->second->closed) {
                ++entry;
                continue;
            }
            std::cout << "closed " << entry->first << " " << static_cast<std::uint32_t>(*entry->second->closed) << '\n';
            std::erase_if(routes_, [id = entry->first](const auto &route) { return route.second == id; });
            ufrags_.erase(entry->second->ufrag);
            entry = sessions_.erase(entry);
        }

        std::optional<Clock::time_point> deadline;
        for (const auto &[id, entry] : sessions_) {
            if (const auto next = entry->session.nextTimeout(); next && (!deadline || *next < *deadline)) {
                deadline = next;
            }
        }
        timer_.expires_at(deadline.value_or(Clock::now() + std::chrono::hours{1}));
        timer_.async_wait([this](const asio::error_code &error) {
            if (error) {
                return;
            }
            for (const auto &[id, entry] : sessions_) {
                if (const auto next = entry->session.nextTimeout(); next && *next <= Clock::now()) {
                    entry->session.handleTimeout(Clock::now());
                }
            }
            settle();
        });
    }

    endgate::EventLoop &event_loop_;
    asio::io_context &io_context_;
    asio::ip::udp::socket socket_;
    asio::steady_timer timer_;
    std::shared_ptr<const endgate::rtc::Certificate> certificate_;
    endgate::nethernet::ServerIdentityAssertionGenerator identity_;
    std::shared_ptr<endgate::nethernet::HttpSignalingServer> signaling_;
    std::map<std::uint64_t, std::unique_ptr<Session>> sessions_;
    std::unordered_map<std::string, std::uint64_t> ufrags_;
    std::map<asio::ip::udp::endpoint, std::uint64_t> routes_;
    std::uint64_t next_id_{1};
    std::array<std::byte, 65536> buffer_{};
    asio::ip::udp::endpoint sender_;
};

}  // namespace

int main(int argc, char **argv)
{
    std::cout << std::unitbuf;
    endgate::EventLoop loop;
    Echo echo(loop, asio::ip::make_address(argc > 1 ? argv[1] : "127.0.0.1"));
    std::cout << "listening " << echo.port() << '\n';
    loop.awaitTermination();
}
