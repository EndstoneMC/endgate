#pragma once

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <spdlog/logger.h>

#include "endgate/awaitable.h"
#include "endgate/connection/connection.h"
#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"
#include "endgate/network/nethernet/client.h"
#include "endgate/network/nethernet/identity.h"
#include "endgate/network/nethernet/session.h"
#include "endgate/network/nethernet/signaling.h"
#include "endgate/network/rtc/certificate.h"
#include "endgate/network/rtc/description.h"
#include "endgate/network/rtc/ice.h"

namespace endgate {

/**
 * A leg carried over NetherNet: a player answered on an endpoint's socket, with a NetworkSession, or
 * a backend the proxy dials from a socket of its own, with a DialerSession. NetherNet carries a batch
 * as it is, so the leg passes batches through unchanged.
 *
 * @see NetherNet::NetworkSession
 */
template <typename Session>
class NetherNetConnection final : public MinecraftConnection, private nethernet::NetworkSession::Listener {
public:
    static constexpr bool DIALING = std::same_as<Session, nethernet::DialerSession>;

    NetherNetConnection(Endpoint &endpoint, const std::string &network_id, const rtc::Description &offer,
                        rtc::IceCredentials local, std::shared_ptr<const rtc::Certificate> certificate,
                        const nethernet::NetworkSession::Config &config, Clock::time_point now)
        requires(!DIALING);

    NetherNetConnection(Endpoint &endpoint, const asio::ip::udp::endpoint &address,
                        std::shared_ptr<const rtc::Certificate> certificate)
        requires DIALING;

    /**
     * Offers the backend a session over HTTP signalling, signed with the server's identity, and runs
     * the session its answer describes.
     *
     * @see NetherNet::ClientNegotiator::createOffer
     */
    void connect(std::string host, std::uint16_t port,
                 std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> identity)
        requires DIALING;

    void handleDatagram(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender) override;
    [[nodiscard]] bool isTransportEncrypted() const override { return true; }

protected:
    void send(ByteBuf batch) override;
    void disconnect(bool wait_for_peer) override;

private:
    void sendDatagram(const asio::ip::udp::endpoint &remote, ByteBufView datagram) override;
    void onRouteAdded(const asio::ip::udp::endpoint &remote) override;
    void onRouteRemoved(const asio::ip::udp::endpoint &remote) override;
    void onOpen() override;
    void onPacket(ByteBufView packet) override;
    void onClosed(nethernet::ESessionError reason) override;

    static Awaitable<void> dial(std::shared_ptr<NetherNetConnection> self, std::string host, std::uint16_t port,
                                std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> identity)
        requires DIALING;
    static Awaitable<void> read(std::shared_ptr<NetherNetConnection> self)
        requires DIALING;
    void finish();
    void arm();

    spdlog::logger &logger_;
    std::shared_ptr<asio::ip::udp::socket> socket_;
    std::shared_ptr<const rtc::Certificate> certificate_;
    asio::ip::udp::endpoint address_;
    asio::steady_timer timer_;
    std::optional<Clock::time_point> armed_;
    bool active_{false};
    bool finished_{false};
    std::optional<Session> session_;
};

template <typename Session>
NetherNetConnection<Session>::NetherNetConnection(Endpoint &endpoint, const std::string &network_id,
                                                  const rtc::Description &offer, rtc::IceCredentials local,
                                                  std::shared_ptr<const rtc::Certificate> certificate,
                                                  const nethernet::NetworkSession::Config &config,
                                                  Clock::time_point now)
    requires(!DIALING)
    : MinecraftConnection(endpoint, network_id, Direction::Serverbound),
      logger_(LogManager::getLogger("NetherNetConnection")), socket_(endpoint.getSocket()), certificate_(certificate),
      timer_(eventLoop().getExecutor())
{
    session_.emplace(static_cast<nethernet::NetworkSession::Listener &>(*this), offer, std::move(local),
                     std::move(certificate), config, now);
}

template <typename Session>
NetherNetConnection<Session>::NetherNetConnection(Endpoint &endpoint, const asio::ip::udp::endpoint &address,
                                                  std::shared_ptr<const rtc::Certificate> certificate)
    requires DIALING
    : MinecraftConnection(endpoint, toString(address), Direction::Clientbound),
      logger_(LogManager::getLogger("NetherNetConnection")),
      socket_(std::make_shared<asio::ip::udp::socket>(eventLoop().getExecutor())), certificate_(std::move(certificate)),
      address_(address), timer_(eventLoop().getExecutor())
{
}

template <typename Session>
void NetherNetConnection<Session>::connect(std::string host, std::uint16_t port,
                                           std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> identity)
    requires DIALING
{
    asio::co_spawn(eventLoop().getExecutor(),
                   dial(std::static_pointer_cast<NetherNetConnection>(shared_from_this()), std::move(host), port,
                        std::move(identity)),
                   asio::detached);
}

template <typename Session>
Awaitable<void> NetherNetConnection<Session>::dial(
    std::shared_ptr<NetherNetConnection> self, std::string host, std::uint16_t port,
    std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> identity)
    requires DIALING
{
    constexpr std::size_t max_packet_size = 8 * 1024 * 1024;

    asio::error_code ec;
    asio::ip::udp::socket route(self->eventLoop().getExecutor());
    asio::ip::udp::endpoint local;
    if (!route.open(self->address_.protocol(), ec) && !route.connect(self->address_, ec) &&
        !self->socket_->open(self->address_.protocol(), ec) &&
        !self->socket_->bind({self->address_.protocol(), 0}, ec) && !self->socket_->non_blocking(true, ec)) {
        local = {route.local_endpoint(ec).address(), self->socket_->local_endpoint(ec).port()};
    }
    if (ec) {
        self->logger_.error("{}: unable to connect: {}", self->getRemoteAddress(), ec.message());
        self->finish();
        co_return;
    }

    std::mt19937_64 random{std::random_device{}()};
    const auto credentials = rtc::IceCredentials::generate();
    auto offer = rtc::Description::offer(credentials, self->certificate_->getFingerprint(), std::array{local})
                     .generate(random());
    if (identity) {
        const auto assertion = nethernet::IdentityAssertion::create(*identity, offer, std::chrono::system_clock::now());
        if (!assertion) {
            self->logger_.error("{}: unable to connect: can't sign the offer", self->getRemoteAddress());
            self->finish();
            co_return;
        }
        offer = assertion->injectIntoSdp(offer);
    }

    const auto answer = co_await nethernet::SignalingClient::requestAnswer(
        self->eventLoop().getIoContext(), std::move(host), port, std::to_string(random()), std::move(offer), {});
    if (self->finished_) {
        co_return;
    }
    if (!answer) {
        self->logger_.error("{}: unable to connect: NetherNet session error {}", self->getRemoteAddress(),
                            static_cast<std::uint32_t>(answer.error()));
        self->finish();
        co_return;
    }
    const auto description = rtc::Description::parse(answer->sdp);
    if (!description) {
        self->logger_.error("{}: unable to connect: unreadable SDP answer", self->getRemoteAddress());
        self->finish();
        co_return;
    }
    self->session_.emplace(static_cast<nethernet::NetworkSession::Listener &>(*self), *description, credentials,
                           self->certificate_, nethernet::DialerSession::Config{.max_packet_size = max_packet_size},
                           Clock::now());
    asio::co_spawn(self->eventLoop().getExecutor(), read(self), asio::detached);
    self->arm();
}

template <typename Session>
Awaitable<void> NetherNetConnection<Session>::read(std::shared_ptr<NetherNetConnection> self)
    requires DIALING
{
    std::array<std::byte, 65536> buffer{};
    asio::ip::udp::endpoint sender;
    for (;;) {
        const auto [ec, size] = co_await self->socket_->async_receive_from(asio::buffer(buffer), sender,
                                                                           asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted || !self->socket_->is_open()) {
            co_return;
        }
        if (ec || size == 0) {
            continue;
        }
        self->handleDatagram(std::span{buffer}.first(size), sender);
    }
}

template <typename Session>
void NetherNetConnection<Session>::handleDatagram(std::span<const std::byte> datagram,
                                                  const asio::ip::udp::endpoint &sender)
{
    if (!session_) {
        return;
    }
    session_->handleDatagram(Clock::now(), sender, Unpooled::wrappedBuffer(datagram));
    channelReadComplete();
    arm();
}

template <typename Session>
void NetherNetConnection<Session>::send(ByteBuf batch)
{
    if (!active_ || !session_) {
        logger_.debug("{}: dropping {} bytes, not connected", getRemoteAddress(), batch.readableBytes());
        return;
    }
    session_->send(batch.slice(), nethernet::ESendType::Reliable, Clock::now());
    arm();
}

template <typename Session>
void NetherNetConnection<Session>::disconnect(bool)
{
    if (!session_) {
        finish();
        return;
    }
    session_->close(Clock::now());
    arm();
}

template <typename Session>
void NetherNetConnection<Session>::sendDatagram(const asio::ip::udp::endpoint &remote, ByteBufView datagram)
{
    asio::error_code ignored;
    socket_->send_to(asio::buffer(datagram.nioBuffer()), remote, 0, ignored);
}

template <typename Session>
void NetherNetConnection<Session>::onRouteAdded(const asio::ip::udp::endpoint &remote)
{
    if constexpr (!DIALING) {
        getEndpoint().addRoute(remote, shared_from_this());
    }
}

template <typename Session>
void NetherNetConnection<Session>::onRouteRemoved(const asio::ip::udp::endpoint &remote)
{
    if constexpr (!DIALING) {
        getEndpoint().removeRoute(remote, shared_from_this());
    }
}

template <typename Session>
void NetherNetConnection<Session>::onOpen()
{
    if constexpr (!DIALING) {
        if (const auto *selected = session_ ? &session_->getIceTransport().getSelectedPair() : nullptr;
            selected != nullptr && selected->has_value()) {
            setRemoteAddress(**selected);
        }
    }
    active_ = true;
    channelActive();
}

template <typename Session>
void NetherNetConnection<Session>::onPacket(ByteBufView packet)
{
    channelRead(packet);
}

template <typename Session>
void NetherNetConnection<Session>::onClosed(nethernet::ESessionError reason)
{
    logger_.debug("{}: connection ended: NetherNet session error {}", getRemoteAddress(),
                  static_cast<std::uint32_t>(reason));
    finish();
}

template <typename Session>
void NetherNetConnection<Session>::finish()
{
    if (finished_) {
        return;
    }
    finished_ = true;
    active_ = false;
    timer_.cancel();
    armed_.reset();
    if constexpr (DIALING) {
        asio::error_code ignored;
        std::ignore = socket_->close(ignored);
    }
    channelInactive();
}

template <typename Session>
void NetherNetConnection<Session>::arm()
{
    const auto next = session_ ? session_->nextTimeout() : std::nullopt;
    if (!next) {
        if (armed_) {
            timer_.cancel();
            armed_.reset();
        }
        return;
    }
    if (armed_ && *armed_ <= *next) {
        return;
    }
    armed_ = next;
    timer_.expires_at(*next);
    timer_.async_wait([weak = std::weak_ptr(std::static_pointer_cast<NetherNetConnection>(shared_from_this()))](
                          const asio::error_code &ec) {
        const auto self = weak.lock();
        if (ec || !self || !self->session_) {
            return;
        }
        self->armed_.reset();
        self->session_->handleTimeout(Clock::now());
        self->channelReadComplete();
        self->arm();
    });
}

}  // namespace endgate
