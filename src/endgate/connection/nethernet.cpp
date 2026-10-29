#include "endgate/connection/nethernet.h"

#include <chrono>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/post.hpp>

#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"

namespace endgate {

NetherNetConnection::NetherNetConnection(Endpoint &endpoint, const std::string &network_id,
                                         const rtc::Description &offer, rtc::IceCredentials local,
                                         std::shared_ptr<const rtc::Certificate> certificate,
                                         const nethernet::NetworkSession::Config &config, Clock::time_point now)
    : MinecraftConnection(endpoint, endpoint.getIoContext(), network_id), logger_(LogManager::getLogger()),
      socket_(endpoint.getSocket()), certificate_(certificate), timer_(getExecutor()),
      session_(*this, offer, std::move(local), std::move(certificate), config, now)
{
}

void NetherNetConnection::answer(rtc::Description offer, std::vector<rtc::CandidateAddress> candidates,
                                 std::uint64_t session_id,
                                 std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> identity,
                                 nethernet::JoinResponse response)
{
    asio::post(getExecutor(), [self = std::static_pointer_cast<NetherNetConnection>(shared_from_this()),
                               offer = std::move(offer), candidates = std::move(candidates), session_id,
                               identity = std::move(identity), response = std::move(response)] {
        if (self->session_.isClosed()) {
            response.reject(static_cast<std::uint32_t>(nethernet::ESessionError::GenericFailure));
            return;
        }
        auto sdp = rtc::Description::answer(offer, self->session_.getIceTransport().getLocalCredentials(),
                                            self->certificate_->getFingerprint(), candidates)
                       .generate(session_id);
        if (identity) {
            const auto assertion =
                nethernet::IdentityAssertion::create(*identity, sdp, std::chrono::system_clock::now());
            if (!assertion) {
                response.reject(static_cast<std::uint32_t>(nethernet::ESessionError::FailedToCreateIdentityAssertion));
                self->session_.close(Clock::now());
                return;
            }
            sdp = assertion->injectIntoSdp(sdp);
        }
        response.answer(std::move(sdp));
        self->arm();
    });
}

void NetherNetConnection::handleDatagram(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender)
{
    session_.handleDatagram(Clock::now(), sender, datagram);
    arm();
}

void NetherNetConnection::send(ByteBuf batch)
{
    if (!active_) {
        logger_.debug("Dropping {} bytes for {}: not connected", batch.length(), getRemoteAddress());
        return;
    }
    session_.send(batch, nethernet::ESendType::Reliable, Clock::now());
    arm();
}

void NetherNetConnection::disconnect(bool)
{
    session_.close(Clock::now());
    arm();
}

void NetherNetConnection::sendDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram)
{
    asio::error_code ignored;
    socket_->send_to(asio::buffer(datagram), remote, 0, ignored);
}

void NetherNetConnection::onRouteAdded(const asio::ip::udp::endpoint &remote)
{
    getEndpoint().addRoute(remote, shared_from_this());
}

void NetherNetConnection::onRouteRemoved(const asio::ip::udp::endpoint &remote)
{
    getEndpoint().removeRoute(remote, shared_from_this());
}

void NetherNetConnection::onOpen()
{
    if (const auto &selected = session_.getIceTransport().getSelectedPair()) {
        setRemoteAddress(toString(*selected));
    }
    active_ = true;
    channelActive();
}

void NetherNetConnection::onPacket(ByteBuf packet)
{
    channelRead(std::move(packet));
}

void NetherNetConnection::onClosed(nethernet::ESessionError reason)
{
    active_ = false;
    timer_.cancel();
    armed_.reset();
    logger_.debug("{} ended: NetherNet session error {}", getRemoteAddress(), static_cast<std::uint32_t>(reason));
    channelInactive();
}

void NetherNetConnection::arm()
{
    const auto next = session_.nextTimeout();
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
        if (ec || !self) {
            return;
        }
        self->armed_.reset();
        self->session_.handleTimeout(Clock::now());
        self->arm();
    });
}

}  // namespace endgate
