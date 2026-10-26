#include "endgate/network/nethernet/session.h"

#include <algorithm>
#include <utility>

namespace endgate::nethernet {

NetworkSession::NetworkSession(Listener &listener, const rtc::Description &offer, rtc::IceCredentials local,
                               std::shared_ptr<const rtc::Certificate> certificate, Config config,
                               Clock::time_point now)
    : listener_(listener), now_(now), ice_(*this, std::move(local), offer.ice_ufrag, config.ice, now),
      dtls_(*this, std::move(certificate),
            offer.setup == rtc::DtlsSetup::Active ? rtc::DtlsTransport::Role::Server : rtc::DtlsTransport::Role::Client,
            offer.fingerprints, config.dtls),
      sctp_(*this, {.dtls_client = offer.setup != rtc::DtlsSetup::Active,
                    .remote_max_message_size = offer.max_message_size,
                    .association = config.association}),
      queue_(*this, config.max_packet_size)
{
}

void NetworkSession::handleDatagram(Clock::time_point now, const asio::ip::udp::endpoint &remote,
                                    std::span<const std::byte> datagram)
{
    now_ = now;
    ++depth_;
    if (!closed_ && !datagram.empty()) {
        const auto first = std::to_integer<std::uint8_t>(datagram.front());
        if (first <= 3) {
            ice_.handleDatagram(now, remote, datagram);
        }
        else if (first >= 20 && first <= 63 && std::ranges::contains(pairs_, remote)) {
            dtls_.handleDatagram(now, datagram);
        }
    }
    --depth_;
    advance();
}

bool NetworkSession::send(const ByteBuf &packet, ESendType type, Clock::time_point now)
{
    if (!isOpen()) {
        return false;
    }
    now_ = now;
    ++depth_;
    const auto pushed = queue_.push(packet, type);
    --depth_;
    advance();
    return pushed;
}

void NetworkSession::close(Clock::time_point now)
{
    now_ = now;
    ++depth_;
    fail(ESessionError::None);
    --depth_;
    advance();
}

std::optional<NetworkSession::Clock::time_point> NetworkSession::nextTimeout() const
{
    if (closed_) {
        return std::nullopt;
    }
    std::optional<Clock::time_point> earliest;
    for (const auto deadline : {ice_.nextTimeout(), dtls_.nextTimeout(),
                                sctp_started_ ? sctp_.nextTimeout() : std::optional<Clock::time_point>{}}) {
        if (deadline && (!earliest || *deadline < *earliest)) {
            earliest = deadline;
        }
    }
    return earliest;
}

void NetworkSession::handleTimeout(Clock::time_point now)
{
    now_ = now;
    ++depth_;
    if (const auto deadline = ice_.nextTimeout(); !closed_ && deadline && *deadline <= now) {
        ice_.handleTimeout(now);
    }
    if (const auto deadline = dtls_.nextTimeout(); !closed_ && deadline && *deadline <= now) {
        dtls_.handleTimeout(now);
    }
    if (const auto deadline = sctp_.nextTimeout(); !closed_ && sctp_started_ && deadline && *deadline <= now) {
        sctp_.handleTimeout(now);
    }
    --depth_;
    advance();
}

void NetworkSession::sendIceDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram)
{
    listener_.sendDatagram(remote, datagram);
}

void NetworkSession::onCandidatePairAdded(const asio::ip::udp::endpoint &remote)
{
    pairs_.push_back(remote);
    listener_.onRouteAdded(remote);
}

void NetworkSession::onCandidatePairRemoved(const asio::ip::udp::endpoint &remote)
{
    std::erase(pairs_, remote);
    listener_.onRouteRemoved(remote);
}

void NetworkSession::onSelectedPairChanged(const std::optional<asio::ip::udp::endpoint> &) {}

void NetworkSession::onIceStateChanged(rtc::IceTransport::State state)
{
    if (state == rtc::IceTransport::State::Failed) {
        fail(ESessionError::ICE);
    }
}

void NetworkSession::sendDtlsDatagram(std::span<const std::byte> datagram)
{
    ice_.send(datagram);
}

void NetworkSession::onDtlsData(std::span<const std::byte> data)
{
    sctp_.handlePacket(data, now_);
}

void NetworkSession::onDtlsStateChanged(rtc::DtlsTransport::State state)
{
    if (state == rtc::DtlsTransport::State::Failed) {
        fail(ESessionError::ICE);
    }
    else if (state == rtc::DtlsTransport::State::Closed) {
        fail(ESessionError::DataChannelClosed);
    }
}

void NetworkSession::sendPacket(std::span<const std::byte> packet)
{
    outbox_.emplace_back(packet.begin(), packet.end());
}

void NetworkSession::onStateChange(rtc::SctpTransport::State state)
{
    if (state == rtc::SctpTransport::State::Disconnected || state == rtc::SctpTransport::State::Failed) {
        fail(ESessionError::DataChannelClosed);
    }
}

void NetworkSession::onChannelOpen(const rtc::DataChannel &channel)
{
    if (channel.label == "ReliableDataChannel") {
        if (!reliable_stream_) {
            reliable_stream_ = channel.stream;
        }
    }
    else if (!unreliable_stream_) {
        unreliable_stream_ = channel.stream;
    }
}

void NetworkSession::onChannelMessage(const rtc::DataChannel &channel, ByteBuf message, bool)
{
    if (channel.stream == reliable_stream_) {
        queue_.onOrderedMessage(message);
    }
    else if (channel.stream == unreliable_stream_) {
        queue_.onUnorderedMessage(message);
    }
}

void NetworkSession::onChannelClosed(const rtc::DataChannel &channel)
{
    if (channel.stream == reliable_stream_ || channel.stream == unreliable_stream_) {
        fail(ESessionError::DataChannelClosed);
    }
}

void NetworkSession::sendFragment(ESendType type, ByteBuf fragment)
{
    const auto stream = type == ESendType::Reliable ? reliable_stream_ : unreliable_stream_;
    if (!stream || closed_) {
        return;
    }
    if (!sctp_.send(*stream, std::move(fragment), true, now_) && type == ESendType::Reliable) {
        fail(ESessionError::GenericFailure);
    }
}

void NetworkSession::onPacket(ByteBuf packet)
{
    if (!closed_) {
        listener_.onPacket(std::move(packet));
    }
}

void NetworkSession::closeChannel(ESendType type)
{
    const auto stream = type == ESendType::Reliable ? reliable_stream_ : unreliable_stream_;
    if (stream) {
        sctp_.closeDataChannel(*stream, now_);
    }
    fail(ESessionError::DataChannelClosed);
}

void NetworkSession::fail(ESessionError reason)
{
    if (!closed_) {
        closed_ = reason;
    }
}

void NetworkSession::advance()
{
    if (depth_ != 0) {
        return;
    }
    ++depth_;
    for (bool progressed = true; progressed && !closed_;) {
        progressed = false;
        if (ice_.getState() == rtc::IceTransport::State::Connected &&
            dtls_.getState() == rtc::DtlsTransport::State::New) {
            dtls_.start(now_);
            progressed = true;
        }
        if (dtls_.getState() == rtc::DtlsTransport::State::Connected && !sctp_started_) {
            sctp_started_ = true;
            sctp_.start(now_);
            progressed = true;
        }
        if (dtls_.getState() == rtc::DtlsTransport::State::Connected && !outbox_.empty()) {
            for (const auto &packet : std::exchange(outbox_, {})) {
                dtls_.send(packet);
            }
            progressed = true;
        }
        if (!open_ && reliable_stream_ && unreliable_stream_) {
            open_ = true;
            if (const auto size = sctp_.maxMessageSize(); size > 1) {
                queue_.setMaxMessageSize(size);
            }
            listener_.onOpen();
            progressed = true;
        }
    }
    if (!closed_ || close_reported_) {
        --depth_;
        return;
    }

    close_reported_ = true;
    if (sctp_started_) {
        sctp_.abort(now_);
    }
    const auto dtls_state = dtls_.getState();
    if (dtls_state == rtc::DtlsTransport::State::Connected) {
        for (const auto &packet : std::exchange(outbox_, {})) {
            dtls_.send(packet);
        }
    }
    if (dtls_state == rtc::DtlsTransport::State::Connecting || dtls_state == rtc::DtlsTransport::State::Connected) {
        dtls_.close();
    }
    outbox_.clear();
    --depth_;
    listener_.onClosed(*closed_);
}

}  // namespace endgate::nethernet
