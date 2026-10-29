#include "endgate/network/rtc/sctp/transport.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace endgate::rtc {

SctpTransport::SctpTransport(Listener &listener, Config config)
    : listener_(listener), config_(std::move(config)), association_(*this, config_.association)
{
}

void SctpTransport::start(Clock::time_point now)
{
    now_ = now;
    association_.connect(now);
}

void SctpTransport::handlePacket(ByteBufView packet, Clock::time_point now)
{
    now_ = now;
    association_.handlePacket(packet, now);
}

void SctpTransport::handleTimeout(Clock::time_point now)
{
    now_ = now;
    association_.handleTimeout(now);
}

std::optional<std::uint16_t> SctpTransport::createDataChannel(std::string label, std::string protocol,
                                                              const Reliability &reliability, std::uint16_t priority,
                                                              Clock::time_point now)
{
    now_ = now;
    constexpr std::size_t max_field = std::numeric_limits<std::uint16_t>::max();
    if (state_ != State::Connected || label.size() > max_field || protocol.size() > max_field ||
        channels_.size() >= config_.max_channels) {
        return std::nullopt;
    }
    const std::uint32_t limit = std::min(association_.outboundStreams(), association_.inboundStreams());
    for (std::uint32_t id = config_.dtls_client ? 0 : 1; id < limit; id += 2) {
        const auto stream = static_cast<std::uint16_t>(id);
        if (channels_.contains(stream)) {
            continue;
        }
        if (!association_.send(stream, static_cast<std::uint32_t>(Ppid::Dcep),
                               DataChannelOpen::of(label, protocol, reliability, priority).serialize(), {}, now)) {
            return std::nullopt;
        }
        channels_.emplace(stream, Channel{
                                      .info = {.stream = stream,
                                               .label = std::move(label),
                                               .protocol = std::move(protocol),
                                               .reliability = reliability,
                                               .priority = priority},
                                      .acknowledged = false,
                                  });
        return stream;
    }
    return std::nullopt;
}

bool SctpTransport::send(std::uint16_t stream, ByteBuf message, bool binary, Clock::time_point now)
{
    now_ = now;
    const auto channel = channels_.find(stream);
    if (state_ != State::Connected || channel == channels_.end() || channel->second.closing ||
        (config_.remote_max_message_size != 0 && message.readableBytes() > config_.remote_max_message_size)) {
        return false;
    }
    const auto &reliability = channel->second.info.reliability;
    const sctp::Sender::SendOptions options{
        .unordered = reliability.unordered && channel->second.acknowledged,
        .max_retransmits = reliability.max_retransmits
                             ? std::optional{static_cast<std::uint16_t>(std::min<std::uint32_t>(
                                   *reliability.max_retransmits, std::numeric_limits<std::uint16_t>::max()))}
                             : std::nullopt,
        .lifetime = reliability.max_packet_life_time,
    };
    if (!message.isReadable()) {
        constexpr std::array<std::byte, 1> placeholder{};
        return association_.send(stream, static_cast<std::uint32_t>(binary ? Ppid::BinaryEmpty : Ppid::StringEmpty),
                                 Unpooled::copiedBuffer(placeholder), options, now);
    }
    return association_.send(stream, static_cast<std::uint32_t>(binary ? Ppid::Binary : Ppid::String),
                             std::move(message), options, now);
}

void SctpTransport::closeDataChannel(std::uint16_t stream, Clock::time_point now)
{
    now_ = now;
    const auto channel = channels_.find(stream);
    if (channel == channels_.end() || channel->second.closing) {
        return;
    }
    channel->second.closing = true;
    const std::array streams{stream};
    if (!association_.resetStreams(streams, now)) {
        finish(stream);
    }
}

void SctpTransport::close(Clock::time_point now)
{
    now_ = now;
    association_.shutdown(now);
}

void SctpTransport::abort(Clock::time_point now)
{
    now_ = now;
    association_.abort(now);
}

const DataChannel *SctpTransport::channel(std::uint16_t stream) const
{
    const auto channel = channels_.find(stream);
    return channel == channels_.end() ? nullptr : &channel->second.info;
}

void SctpTransport::onEstablished()
{
    changeState(State::Connected);
}

void SctpTransport::onRestarted()
{
    closeAll();
}

void SctpTransport::onClosed(sctp::Association::CloseReason reason)
{
    changeState(reason == sctp::Association::CloseReason::Shutdown ? State::Disconnected : State::Failed);
    closeAll();
}

void SctpTransport::onMessage(std::uint16_t stream, std::uint32_t ppid, bool, ByteBufView payload)
{
    const auto channel = channels_.find(stream);
    switch (static_cast<Ppid>(ppid)) {
    case Ppid::Dcep: {
        if (!payload.isReadable()) {
            return;
        }
        const auto type = payload.getUnsignedByte(payload.readerIndex());
        if (type == DataChannelOpen::MESSAGE_TYPE) {
            handleOpen(stream, payload);
        }
        else if (type == DataChannelAck::MESSAGE_TYPE && channel != channels_.end()) {
            channel->second.acknowledged = true;
        }
        return;
    }
    case Ppid::String:
    case Ppid::Binary:
    case Ppid::StringEmpty:
    case Ppid::BinaryEmpty: {
        if (channel == channels_.end()) {
            refuse(stream);
            return;
        }
        channel->second.acknowledged = true;
        const auto kind = static_cast<Ppid>(ppid);
        const auto empty = kind == Ppid::StringEmpty || kind == Ppid::BinaryEmpty;
        notifying_ = stream;
        listener_.onChannelMessage(channel->second.info, empty ? ByteBufView{} : payload,
                                   kind == Ppid::Binary || kind == Ppid::BinaryEmpty);
        notifying_.reset();
        if (std::exchange(finish_after_notify_, false)) {
            finish(stream);
        }
        return;
    }
    default:
        if (channel != channels_.end()) {
            closeDataChannel(stream, now_);
        }
        return;
    }
}

void SctpTransport::onIncomingStreamsReset(std::span<const std::uint16_t> streams)
{
    std::vector<std::uint16_t> affected(streams.begin(), streams.end());
    if (affected.empty()) {
        for (const auto &[stream, channel] : channels_) {
            affected.push_back(stream);
        }
    }
    for (const auto stream : affected) {
        const auto channel = channels_.find(stream);
        if (channel == channels_.end()) {
            continue;
        }
        channel->second.incoming_reset = true;
        if (!channel->second.closing) {
            channel->second.closing = true;
            const std::array reset{stream};
            if (!association_.resetStreams(reset, now_)) {
                channel->second.outgoing_reset = true;
            }
        }
        if (channel->second.outgoing_reset) {
            finish(stream);
        }
    }
}

void SctpTransport::onOutgoingStreamsReset(std::span<const std::uint16_t> streams, bool performed)
{
    for (const auto stream : streams) {
        const auto channel = channels_.find(stream);
        if (channel == channels_.end()) {
            continue;
        }
        channel->second.outgoing_reset = true;
        if (channel->second.incoming_reset || !performed) {
            finish(stream);
        }
    }
}

void SctpTransport::handleOpen(std::uint16_t stream, ByteBufView message)
{
    const auto open = DataChannelOpen::parse(message);
    const auto reliability = open ? open->reliability() : std::nullopt;
    const auto peer_parity = config_.dtls_client ? 1U : 0U;
    if (!reliability || stream % 2U != peer_parity || channels_.contains(stream) ||
        channels_.size() >= config_.max_channels || open->label.size() > config_.max_label_size ||
        open->protocol.size() > config_.max_label_size || stream >= association_.outboundStreams()) {
        refuse(stream);
        return;
    }
    constexpr std::array ack{std::byte{DataChannelAck::MESSAGE_TYPE}};
    if (!association_.send(stream, static_cast<std::uint32_t>(Ppid::Dcep), Unpooled::copiedBuffer(ack), {}, now_)) {
        refuse(stream);
        return;
    }
    const auto &channel = channels_
                              .emplace(stream,
                                       Channel{
                                           .info = {.stream = stream,
                                                    .label = open->label,
                                                    .protocol = open->protocol,
                                                    .reliability = *reliability,
                                                    .priority = open->priority},
                                           .acknowledged = true,
                                       })
                              .first->second;
    notifying_ = stream;
    listener_.onChannelOpen(channel.info);
    notifying_.reset();
    if (std::exchange(finish_after_notify_, false)) {
        finish(stream);
    }
}

void SctpTransport::refuse(std::uint16_t stream)
{
    const auto channel = channels_.find(stream);
    if (channel != channels_.end()) {
        closeDataChannel(stream, now_);
        return;
    }
    const std::array streams{stream};
    association_.resetStreams(streams, now_);
}

void SctpTransport::finish(std::uint16_t stream)
{
    if (notifying_ == stream) {
        finish_after_notify_ = true;
        return;
    }
    const auto channel = channels_.find(stream);
    if (channel == channels_.end()) {
        return;
    }
    auto info = std::move(channel->second.info);
    channels_.erase(channel);
    listener_.onChannelClosed(info);
}

void SctpTransport::closeAll()
{
    auto channels = std::exchange(channels_, {});
    for (auto &[stream, channel] : channels) {
        listener_.onChannelClosed(channel.info);
    }
}

void SctpTransport::changeState(State state)
{
    if (state_ != state) {
        state_ = state;
        listener_.onStateChange(state);
    }
}

}  // namespace endgate::rtc
