#include "endgate/network/nethernet/session.h"

#include <algorithm>
#include <system_error>
#include <utility>

#include <asio/ip/address.hpp>

namespace endgate::nethernet {

NetworkSession::NetworkSession(Listener &listener, const rtc::Description &offer, rtc::IceCredentials local,
                               std::shared_ptr<const rtc::Certificate> certificate, const Config &config,
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
            ice_.handleDatagram(now, remote, Unpooled::wrappedBuffer(datagram));
        }
        else if (first >= 20 && first <= 63 && std::ranges::contains(pairs_, remote)) {
            dtls_.handleDatagram(now, Unpooled::wrappedBuffer(datagram));
        }
    }
    --depth_;
    advance();
}

bool NetworkSession::send(const LegacyByteBuf &packet, ESendType type, Clock::time_point now)
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

void NetworkSession::sendIceDatagram(const asio::ip::udp::endpoint &remote, ByteBufView datagram)
{
    listener_.sendDatagram(remote, datagram.nioBuffer());
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

void NetworkSession::sendDtlsDatagram(ByteBufView datagram)
{
    ice_.send(datagram);
}

void NetworkSession::onDtlsData(ByteBufView data)
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

void NetworkSession::sendPacket(ByteBufView packet)
{
    const auto bytes = packet.nioBuffer();
    outbox_.emplace_back(bytes.begin(), bytes.end());
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

void NetworkSession::onChannelMessage(const rtc::DataChannel &channel, ByteBufView message, bool)
{
    if (channel.stream == reliable_stream_) {
        queue_.onOrderedMessage(LegacyByteBuf::copyBuffer(message.nioBuffer()));
    }
    else if (channel.stream == unreliable_stream_) {
        queue_.onUnorderedMessage(LegacyByteBuf::copyBuffer(message.nioBuffer()));
    }
}

void NetworkSession::onChannelClosed(const rtc::DataChannel &channel)
{
    if (channel.stream == reliable_stream_ || channel.stream == unreliable_stream_) {
        fail(ESessionError::DataChannelClosed);
    }
}

void NetworkSession::sendFragment(ESendType type, LegacyByteBuf fragment)
{
    const auto stream = type == ESendType::Reliable ? reliable_stream_ : unreliable_stream_;
    if (!stream || closed_) {
        return;
    }
    auto message = Unpooled::buffer(fragment.length());
    message.writeZero(fragment.length());
    fragment.copyTo(message.nioBuffer());
    if (!sctp_.send(*stream, std::move(message), true, now_) && type == ESendType::Reliable) {
        fail(ESessionError::GenericFailure);
    }
}

void NetworkSession::onPacket(LegacyByteBuf packet)
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
                dtls_.send(Unpooled::wrappedBuffer(packet));
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
            dtls_.send(Unpooled::wrappedBuffer(packet));
        }
    }
    if (dtls_state == rtc::DtlsTransport::State::Connecting || dtls_state == rtc::DtlsTransport::State::Connected) {
        dtls_.close();
    }
    outbox_.clear();
    --depth_;
    listener_.onClosed(*closed_);
}

std::vector<asio::ip::udp::endpoint> DialerSession::endpointsOf(const rtc::Description &answer)
{
    std::vector<asio::ip::udp::endpoint> endpoints;
    for (const auto &candidate : answer.candidates) {
        if (candidate.transport != "udp") {
            continue;
        }
        std::error_code error;
        const auto address = asio::ip::make_address(candidate.address, error);
        if (!error &&
            std::ranges::find(endpoints, asio::ip::udp::endpoint{address, candidate.port}) == endpoints.end()) {
            endpoints.emplace_back(address, candidate.port);
        }
    }
    return endpoints;
}

DialerSession::DialerSession(Listener &listener, const rtc::Description &answer, rtc::IceCredentials local,
                             std::shared_ptr<const rtc::Certificate> certificate, const Config &config,
                             Clock::time_point now)
    : listener_(listener), now_(now), candidates_(endpointsOf(answer)),
      ice_(*this, std::move(local), answer.ice_ufrag, answer.ice_pwd, candidates_,
           rtc::IceController::generateTieBreaker(), config.ice, now),
      dtls_(*this, std::move(certificate),
            answer.setup == rtc::DtlsSetup::Active ? rtc::DtlsTransport::Role::Server
                                                   : rtc::DtlsTransport::Role::Client,
            answer.fingerprints, config.dtls),
      sctp_(*this, {.dtls_client = answer.setup != rtc::DtlsSetup::Active,
                    .remote_max_message_size = answer.max_message_size,
                    .association = config.association}),
      queue_(*this, config.max_packet_size)
{
    for (const auto &remote : candidates_) {
        listener_.onRouteAdded(remote);
    }
}

void DialerSession::handleDatagram(Clock::time_point now, const asio::ip::udp::endpoint &remote,
                                   std::span<const std::byte> datagram)
{
    now_ = now;
    ++depth_;
    if (!closed_ && !datagram.empty()) {
        const auto first = std::to_integer<std::uint8_t>(datagram.front());
        if (first <= 3) {
            ice_.handleDatagram(now, remote, Unpooled::wrappedBuffer(datagram));
        }
        else if (first >= 20 && first <= 63 && std::ranges::contains(candidates_, remote)) {
            dtls_.handleDatagram(now, Unpooled::wrappedBuffer(datagram));
        }
    }
    --depth_;
    advance();
}

bool DialerSession::send(const LegacyByteBuf &packet, ESendType type, Clock::time_point now)
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

void DialerSession::close(Clock::time_point now)
{
    now_ = now;
    ++depth_;
    fail(ESessionError::None);
    --depth_;
    advance();
}

std::optional<DialerSession::Clock::time_point> DialerSession::nextTimeout() const
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

void DialerSession::handleTimeout(Clock::time_point now)
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

void DialerSession::sendIceDatagram(const asio::ip::udp::endpoint &remote, ByteBufView datagram)
{
    listener_.sendDatagram(remote, datagram.nioBuffer());
}

void DialerSession::onSelectedPairChanged(const std::optional<asio::ip::udp::endpoint> &) {}

void DialerSession::onIceStateChanged(rtc::IceController::State state)
{
    if (state == rtc::IceController::State::Failed) {
        fail(ESessionError::ICE);
    }
}

void DialerSession::sendDtlsDatagram(ByteBufView datagram)
{
    ice_.send(datagram);
}

void DialerSession::onDtlsData(ByteBufView data)
{
    sctp_.handlePacket(data, now_);
}

void DialerSession::onDtlsStateChanged(rtc::DtlsTransport::State state)
{
    if (state == rtc::DtlsTransport::State::Failed) {
        fail(ESessionError::ICE);
    }
    else if (state == rtc::DtlsTransport::State::Closed) {
        fail(ESessionError::DataChannelClosed);
    }
}

void DialerSession::sendPacket(ByteBufView packet)
{
    const auto bytes = packet.nioBuffer();
    outbox_.emplace_back(bytes.begin(), bytes.end());
}

void DialerSession::onStateChange(rtc::SctpTransport::State state)
{
    if (state == rtc::SctpTransport::State::Disconnected || state == rtc::SctpTransport::State::Failed) {
        fail(ESessionError::DataChannelClosed);
    }
}

void DialerSession::onChannelOpen(const rtc::DataChannel &) {}

void DialerSession::onChannelMessage(const rtc::DataChannel &channel, ByteBufView message, bool)
{
    if (channel.stream == reliable_stream_) {
        queue_.onOrderedMessage(LegacyByteBuf::copyBuffer(message.nioBuffer()));
    }
    else if (channel.stream == unreliable_stream_) {
        queue_.onUnorderedMessage(LegacyByteBuf::copyBuffer(message.nioBuffer()));
    }
}

void DialerSession::onChannelClosed(const rtc::DataChannel &channel)
{
    if (channel.stream == reliable_stream_ || channel.stream == unreliable_stream_) {
        fail(ESessionError::DataChannelClosed);
    }
}

void DialerSession::sendFragment(ESendType type, LegacyByteBuf fragment)
{
    const auto stream = type == ESendType::Reliable ? reliable_stream_ : unreliable_stream_;
    if (!stream || closed_) {
        return;
    }
    auto message = Unpooled::buffer(fragment.length());
    message.writeZero(fragment.length());
    fragment.copyTo(message.nioBuffer());
    if (!sctp_.send(*stream, std::move(message), true, now_) && type == ESendType::Reliable) {
        fail(ESessionError::GenericFailure);
    }
}

void DialerSession::onPacket(LegacyByteBuf packet)
{
    if (!closed_) {
        listener_.onPacket(std::move(packet));
    }
}

void DialerSession::closeChannel(ESendType type)
{
    const auto stream = type == ESendType::Reliable ? reliable_stream_ : unreliable_stream_;
    if (stream) {
        sctp_.closeDataChannel(*stream, now_);
    }
    fail(ESessionError::DataChannelClosed);
}

void DialerSession::fail(ESessionError reason)
{
    if (!closed_) {
        closed_ = reason;
    }
}

void DialerSession::advance()
{
    if (depth_ != 0) {
        return;
    }
    ++depth_;
    for (bool progressed = true; progressed && !closed_;) {
        progressed = false;
        if (ice_.getState() == rtc::IceController::State::Connected &&
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
                dtls_.send(Unpooled::wrappedBuffer(packet));
            }
            progressed = true;
        }
        if (sctp_.state() == rtc::SctpTransport::State::Connected && !channels_opened_) {
            channels_opened_ = true;
            reliable_stream_ = sctp_.createDataChannel("ReliableDataChannel", "", {}, 0, now_);
            unreliable_stream_ = sctp_.createDataChannel("UnreliableDataChannel", "",
                                                         {.unordered = true, .max_retransmits = 0}, 0, now_);
            if (!reliable_stream_ || !unreliable_stream_) {
                fail(ESessionError::GenericFailure);
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
            dtls_.send(Unpooled::wrappedBuffer(packet));
        }
    }
    if (dtls_state == rtc::DtlsTransport::State::Connecting || dtls_state == rtc::DtlsTransport::State::Connected) {
        dtls_.close();
    }
    outbox_.clear();
    for (const auto &remote : candidates_) {
        listener_.onRouteRemoved(remote);
    }
    --depth_;
    listener_.onClosed(*closed_);
}

}  // namespace endgate::nethernet
