#include "endgate/network/rtc/sctp/association.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include <openssl/rand.h>

namespace endgate::rtc::sctp {
namespace {

constexpr std::uint8_t reflected_tag = 0x01;

std::vector<std::byte> cause(CauseCode code, std::span<const std::byte> value = {})
{
    std::vector<std::byte> out;
    ByteWriter(out).writeTlv(static_cast<std::uint16_t>(code), value);
    return out;
}

std::size_t padded(std::size_t length)
{
    return (length + 3) & ~std::size_t{3};
}

}  // namespace

Association::Association(Listener &listener, Config config) : listener_(listener), config_(std::move(config))
{
    if (!config_.random) {
        config_.random = [](std::span<std::byte> out) {
            if (RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), static_cast<int>(out.size())) != 1) {
                throw std::runtime_error("RAND_bytes failed");
            }
        };
    }
    config_.random(cookie_key_);
}

template <typename T>
T Association::random()
{
    std::array<std::byte, sizeof(T)> bytes{};
    config_.random(bytes);
    T value = 0;
    for (const auto byte : bytes) {
        value = static_cast<T>(value << 8U | std::to_integer<T>(byte));
    }
    return value;
}

void Association::connect(Clock::time_point now)
{
    if (terminated_ || state_ != State::Closed) {
        return;
    }
    init_tag_ = random<std::uint32_t>();
    while (init_tag_ == 0) {
        init_tag_ = random<std::uint32_t>();
    }
    init_tsn_ = random<std::uint32_t>();
    state_ = State::CookieWait;
    t1_ = Retry{.at = now + config_.init_timeout, .backoff = config_.init_timeout, .count = 0};
    sendInit();
    deliver();
}

void Association::handlePacket(std::span<const std::byte> data, Clock::time_point now)
{
    const auto packet = terminated_ ? std::nullopt : Packet::parse(data);
    if (!packet || packet->header.destination_port != config_.local_port ||
        packet->header.source_port != config_.remote_port || !verify(*packet)) {
        ++dropped_packets_;
        return;
    }
    dispatch(*packet, data, now);
    if (tcb_) {
        tcb_->receiver.endPacket(now, std::min(config_.delayed_ack_timeout, tcb_->sender.rto() / 2));
        if (const auto failure = tcb_->receiver.failure()) {
            fail(*failure,
                 *failure == CauseCode::OutOfResource ? CloseReason::OutOfResource : CloseReason::ProtocolViolation);
        }
    }
    transmit(now);
    deliver();
}

void Association::handleTimeout(Clock::time_point now)
{
    if (terminated_) {
        return;
    }
    if (t1_ && now >= t1_->at) {
        if (++t1_->count > config_.max_init_retransmits) {
            close(CloseReason::Timeout);
            deliver();
            return;
        }
        t1_->backoff = std::min(t1_->backoff * 2, config_.rto_max);
        t1_->at = now + t1_->backoff;
        if (state_ == State::CookieWait) {
            sendInit();
        }
        else {
            sendCookieEcho();
        }
    }
    if (t2_ && now >= t2_->at) {
        if (++t2_->count > config_.max_retransmissions) {
            sendStandalone(tcb_->peer_tag, ChunkType::Abort, 0, cause(CauseCode::UserInitiatedAbort));
            close(CloseReason::Timeout);
            deliver();
            return;
        }
        t2_->backoff = std::min(t2_->backoff * 2, config_.rto_max);
        t2_->at = now + t2_->backoff;
        if (state_ == State::ShutdownAckSent) {
            queue(ChunkType::ShutdownAck, 0, {});
        }
        else {
            std::vector<std::byte> value;
            ByteWriter(value).write32(tcb_->receiver.cumulativeTsn());
            queue(ChunkType::Shutdown, 0, std::move(value));
        }
    }
    if (tcb_) {
        tcb_->receiver.handleTimeout(now);
        if (tcb_->sender.handleTimeout(now)) {
            countError();
        }
    }
    if (tcb_ && tcb_->heartbeat_timeout && now >= *tcb_->heartbeat_timeout) {
        tcb_->heartbeat_timeout.reset();
        countError();
    }
    if (tcb_ && tcb_->heartbeat_at && now >= *tcb_->heartbeat_at) {
        if (!tcb_->heartbeat_timeout && state_ == State::Established) {
            tcb_->heartbeat_nonce = random<std::uint64_t>();
            std::vector<std::byte> info;
            ByteWriter info_writer(info);
            info_writer.write64(static_cast<std::uint64_t>(now.time_since_epoch().count()));
            info_writer.write64(tcb_->heartbeat_nonce);
            std::vector<std::byte> value;
            ByteWriter(value).writeTlv(static_cast<std::uint16_t>(ParameterType::HeartbeatInfo), info);
            queue(ChunkType::Heartbeat, 0, std::move(value));
            tcb_->heartbeat_timeout = now + tcb_->sender.rto();
        }
        tcb_->heartbeat_at = now + config_.heartbeat_interval + tcb_->sender.rto();
    }
    if (tcb_ && tcb_->request && tcb_->reconfig_at && now >= *tcb_->reconfig_at) {
        if (!std::exchange(tcb_->request->in_progress, false)) {
            countError();
        }
        if (tcb_) {
            tcb_->reconfig_backoff = std::min(tcb_->reconfig_backoff * 2, config_.rto_max);
            tcb_->reconfig_at = now + tcb_->reconfig_backoff;
            queueResetRequest();
        }
    }
    transmit(now);
    deliver();
}

std::optional<Association::Clock::time_point> Association::nextTimeout() const
{
    std::optional<Clock::time_point> next;
    const auto consider = [&](std::optional<Clock::time_point> deadline) {
        if (deadline && (!next || *deadline < *next)) {
            next = deadline;
        }
    };
    consider(t1_ ? std::optional{t1_->at} : std::nullopt);
    consider(t2_ ? std::optional{t2_->at} : std::nullopt);
    if (tcb_) {
        consider(tcb_->receiver.nextTimeout());
        consider(tcb_->sender.nextTimeout());
        consider(tcb_->heartbeat_at);
        consider(tcb_->heartbeat_timeout);
        consider(tcb_->request ? tcb_->reconfig_at : std::nullopt);
    }
    return next;
}

bool Association::send(std::uint16_t stream, std::uint32_t ppid, ByteBuf payload, const Sender::SendOptions &options,
                       Clock::time_point now)
{
    if (!tcb_ || state_ != State::Established || stream >= tcb_->outbound_streams ||
        !tcb_->sender.send(stream, ppid, std::move(payload), options, now)) {
        return false;
    }
    transmit(now);
    deliver();
    return true;
}

bool Association::resetStreams(std::span<const std::uint16_t> streams, Clock::time_point now)
{
    if (!tcb_ || !tcb_->reconfig || state_ != State::Established) {
        return false;
    }
    for (const auto stream : streams) {
        if (stream < tcb_->outbound_streams) {
            tcb_->sender.prepareReset(stream);
        }
    }
    transmit(now);
    deliver();
    return true;
}

void Association::shutdown(Clock::time_point now)
{
    if (terminated_) {
        return;
    }
    if (state_ == State::Closed || state_ == State::CookieWait) {
        close(CloseReason::Shutdown);
    }
    else if (state_ == State::CookieEchoed) {
        sendStandalone(tcb_->peer_tag, ChunkType::Abort, 0, cause(CauseCode::UserInitiatedAbort));
        close(CloseReason::Shutdown);
    }
    else if (state_ == State::Established) {
        state_ = State::ShutdownPending;
        maybeFinishShutdown(now);
        transmit(now);
    }
    deliver();
}

void Association::abort(Clock::time_point)
{
    if (terminated_) {
        return;
    }
    if (tcb_) {
        sendStandalone(tcb_->peer_tag, ChunkType::Abort, 0, cause(CauseCode::UserInitiatedAbort));
    }
    close(CloseReason::Aborted);
    deliver();
}

Association::InitParameters Association::readParameters(std::span<const Tlv> parameters)
{
    InitParameters result{.state_cookie = std::nullopt, .capabilities = {false, false}, .unrecognized = {}};
    for (const auto &parameter : parameters) {
        switch (static_cast<ParameterType>(parameter.type)) {
        case ParameterType::StateCookie:
            result.state_cookie = parameter.value;
            continue;
        case ParameterType::ForwardTsnSupported:
            result.capabilities.partial_reliability = true;
            continue;
        case ParameterType::SupportedExtensions:
            for (const auto type : parameter.value) {
                result.capabilities.partial_reliability |= type == std::byte{0xC0};
                result.capabilities.reconfig |= type == std::byte{0x82};
            }
            continue;
        case ParameterType::Ipv4Address:
        case ParameterType::Ipv6Address:
        case ParameterType::CookiePreservative:
        case ParameterType::HostNameAddress:
        case ParameterType::SupportedAddressTypes:
            continue;
        default:
            break;
        }
        const auto action = parameter.type >> 14U;
        if (action == 1 || action == 3) {
            result.unrecognized.push_back(parameter.raw);
        }
        if (action < 2) {
            break;
        }
    }
    return result;
}

void Association::writeCapabilities(ByteWriter &writer) const
{
    constexpr std::array extensions{std::byte{static_cast<std::uint8_t>(ChunkType::ReConfig)},
                                    std::byte{static_cast<std::uint8_t>(ChunkType::ForwardTsn)}};
    writer.writeTlv(static_cast<std::uint16_t>(ParameterType::ForwardTsnSupported), {});
    writer.writeTlv(static_cast<std::uint16_t>(ParameterType::SupportedExtensions), extensions);
}

bool Association::verify(const Packet &packet) const
{
    const auto &first = packet.chunks.front();
    const auto tag = packet.header.verification_tag;
    const auto has_init = std::ranges::any_of(
        packet.chunks, [](const Chunk &chunk) { return chunk.type == static_cast<std::uint8_t>(ChunkType::Init); });
    if (has_init) {
        return packet.chunks.size() == 1 && tag == 0;
    }
    const auto local = tcb_ ? tcb_->local_tag : (state_ == State::CookieWait ? init_tag_ : 0);
    switch (static_cast<ChunkType>(first.type)) {
    case ChunkType::Abort:
    case ChunkType::ShutdownComplete:
        if ((first.flags & reflected_tag) != 0) {
            return !tcb_ || tag == tcb_->peer_tag;
        }
        return local != 0 && tag == local;
    case ChunkType::InitAck:
        return state_ == State::CookieWait && tag == init_tag_;
    case ChunkType::CookieEcho:
        return true;
    case ChunkType::ShutdownAck:
        if (state_ == State::CookieWait || state_ == State::CookieEchoed) {
            return true;
        }
        break;
    default:
        break;
    }
    return local != 0 && tag == local;
}

void Association::dispatch(const Packet &packet, std::span<const std::byte> data, Clock::time_point now)
{
    std::optional<ByteBuf> copy;
    std::vector<Message> delivered;
    std::vector<std::byte> unrecognized;
    auto data_seen = false;
    const auto cookie_first = packet.chunks.front().type == static_cast<std::uint8_t>(ChunkType::CookieEcho);

    for (std::size_t index = 0; index < packet.chunks.size() && !terminated_; ++index) {
        const auto &chunk = packet.chunks[index];
        if (index > 0 && cookie_first && (!tcb_ || packet.header.verification_tag != tcb_->local_tag)) {
            break;
        }
        auto stop = false;
        switch (static_cast<ChunkType>(chunk.type)) {
        case ChunkType::Data:
            data_seen = true;
            handleData(chunk, data, copy, delivered);
            break;
        case ChunkType::Init:
            handleInit(chunk, now);
            break;
        case ChunkType::InitAck:
            handleInitAck(chunk, now);
            break;
        case ChunkType::Sack:
            handleSack(chunk, now);
            break;
        case ChunkType::Heartbeat:
            if (tcb_) {
                queue(ChunkType::HeartbeatAck, 0, {chunk.value.begin(), chunk.value.end()});
            }
            break;
        case ChunkType::HeartbeatAck:
            handleHeartbeatAck(chunk, now);
            break;
        case ChunkType::Abort:
            if (tcb_ || state_ == State::CookieWait) {
                close(CloseReason::PeerAborted);
            }
            stop = true;
            break;
        case ChunkType::Shutdown:
            if (chunk.value.size() == 4) {
                handleShutdown(now);
            }
            break;
        case ChunkType::ShutdownAck:
            handleShutdownAck(packet);
            stop = true;
            break;
        case ChunkType::Error:
            handleError(chunk);
            break;
        case ChunkType::CookieEcho:
            stop = !handleCookieEcho(packet, chunk, now);
            break;
        case ChunkType::CookieAck:
            if (state_ == State::CookieEchoed) {
                becomeEstablished(now);
            }
            break;
        case ChunkType::ShutdownComplete:
            if (state_ == State::ShutdownAckSent) {
                close(CloseReason::Shutdown);
            }
            stop = true;
            break;
        case ChunkType::ReConfig:
            handleReconfig(chunk, now);
            break;
        case ChunkType::ForwardTsn:
            data_seen = true;
            handleForwardTsn(chunk, delivered);
            break;
        default:
            stop = !handleUnknown(chunk, unrecognized);
            break;
        }

        for (auto &message : delivered) {
            events_.emplace_back(std::move(message));
        }
        delivered.clear();
        if (tcb_ && tcb_->receiver.resetDeferred()) {
            if (auto streams = tcb_->receiver.completeDeferredReset(delivered)) {
                if (tcb_->deferred_request == tcb_->last_peer_request) {
                    tcb_->last_peer_result = ReconfigResult::SuccessPerformed;
                }
                tcb_->deferred_request.reset();
                events_.emplace_back(IncomingReset{.streams = std::move(*streams)});
                for (auto &message : delivered) {
                    events_.emplace_back(std::move(message));
                }
                delivered.clear();
            }
        }
        if (stop) {
            break;
        }
    }

    if (!unrecognized.empty() && tcb_) {
        queue(ChunkType::Error, 0, std::move(unrecognized));
    }
    if (data_seen && state_ == State::ShutdownSent) {
        sendShutdown(now);
    }
}

void Association::handleInit(const Chunk &chunk, Clock::time_point now)
{
    const auto init = InitChunk::parse(chunk);
    if (!init) {
        ++dropped_packets_;
        return;
    }
    if (init->initiate_tag == 0) {
        return;
    }
    if (init->outbound_streams == 0 || init->inbound_streams == 0 || init->a_rwnd < 1500) {
        sendStandalone(init->initiate_tag, ChunkType::Abort, 0, cause(CauseCode::InvalidMandatoryParameter));
        return;
    }
    if (state_ == State::ShutdownAckSent) {
        queue(ChunkType::ShutdownAck, 0, {});
        return;
    }

    const auto parameters = readParameters(init->parameters);
    std::uint32_t local_tag = 0;
    std::uint32_t local_tsn = 0;
    std::uint64_t tie_tag = 0;
    if (state_ == State::CookieWait) {
        local_tag = init_tag_;
        local_tsn = init_tsn_;
    }
    else if (state_ == State::CookieEchoed) {
        local_tag = tcb_->local_tag;
        local_tsn = tcb_->local_initial_tsn;
        tie_tag = tcb_->tie_tag;
    }
    else {
        while (local_tag == 0 || (tcb_ && local_tag == tcb_->local_tag)) {
            local_tag = random<std::uint32_t>();
        }
        local_tsn = random<std::uint32_t>();
        tie_tag = tcb_ ? tcb_->tie_tag : 0;
    }

    const StateCookie cookie{
        .local_tag = local_tag,
        .peer_tag = init->initiate_tag,
        .local_initial_tsn = local_tsn,
        .peer_initial_tsn = init->initial_tsn,
        .peer_a_rwnd = init->a_rwnd,
        .peer_outbound_streams = init->outbound_streams,
        .peer_inbound_streams = init->inbound_streams,
        .tie_tag = tie_tag,
        .peer_supports_forward_tsn = parameters.capabilities.partial_reliability,
        .peer_supports_reconfig = parameters.capabilities.reconfig,
    };
    std::vector<std::byte> value;
    ByteWriter writer(value);
    writer.writeTlv(static_cast<std::uint16_t>(ParameterType::StateCookie), cookie.seal(cookie_key_, now));
    writeCapabilities(writer);
    for (const auto unrecognized : parameters.unrecognized) {
        if (CommonHeader::size + Chunk::header_size + 16 + padded(value.size()) + Tlv::header_size +
                unrecognized.size() >
            config_.mtu) {
            break;
        }
        writer.writeTlv(static_cast<std::uint16_t>(ParameterType::UnrecognizedParameter), unrecognized);
    }

    PacketBuilder builder(buffer_, {config_.local_port, config_.remote_port, init->initiate_tag},
                          std::numeric_limits<std::uint16_t>::max());
    builder.addInit(ChunkType::InitAck,
                    {.initiate_tag = local_tag,
                     .a_rwnd = static_cast<std::uint32_t>(config_.receive_window),
                     .outbound_streams = config_.streams,
                     .inbound_streams = config_.streams,
                     .initial_tsn = local_tsn,
                     .parameters = {}},
                    value);
    listener_.sendPacket(builder.build());
}

void Association::handleInitAck(const Chunk &chunk, Clock::time_point now)
{
    if (state_ != State::CookieWait) {
        return;
    }
    const auto init_ack = InitChunk::parse(chunk);
    if (!init_ack) {
        ++dropped_packets_;
        return;
    }
    const auto parameters = readParameters(init_ack->parameters);
    if (init_ack->initiate_tag == 0 || init_ack->outbound_streams == 0 || init_ack->inbound_streams == 0 ||
        !parameters.state_cookie ||
        parameters.state_cookie->size() + CommonHeader::size + Chunk::header_size > config_.mtu) {
        if (init_ack->initiate_tag != 0) {
            sendStandalone(init_ack->initiate_tag, ChunkType::Abort, 0,
                           parameters.state_cookie
                               ? cause(CauseCode::InvalidMandatoryParameter)
                               : cause(CauseCode::MissingMandatoryParameter,
                                       std::array<std::byte, 6>{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
                                                                std::byte{0}, std::byte{7}}));
        }
        close(CloseReason::ProtocolViolation);
        return;
    }
    tcb_.emplace(makeTcb(init_tag_, init_tsn_, init_ack->initiate_tag, init_ack->initial_tsn, init_ack->a_rwnd,
                         init_ack->outbound_streams, init_ack->inbound_streams, parameters.capabilities));
    cookie_echo_.assign(parameters.state_cookie->begin(), parameters.state_cookie->end());
    state_ = State::CookieEchoed;
    t1_ = Retry{.at = now + config_.init_timeout, .backoff = config_.init_timeout, .count = 0};
    sendCookieEcho();
}

bool Association::handleCookieEcho(const Packet &packet, const Chunk &chunk, Clock::time_point now)
{
    const auto opened = StateCookie::open(cookie_key_, chunk.value, now, config_.cookie_lifetime);
    if (!opened || packet.header.verification_tag != opened->cookie.local_tag) {
        ++dropped_packets_;
        return false;
    }
    const auto &cookie = opened->cookie;
    const auto stale = [&] {
        std::vector<std::byte> staleness;
        const auto excess =
            std::chrono::duration_cast<std::chrono::microseconds>(opened->age - config_.cookie_lifetime);
        ByteWriter(staleness).write32(static_cast<std::uint32_t>(
            std::min<std::int64_t>(excess.count(), std::numeric_limits<std::uint32_t>::max())));
        sendStandalone(cookie.peer_tag, ChunkType::Error, 0, cause(CauseCode::StaleCookie, staleness));
        return false;
    };

    if (!tcb_) {
        if (opened->expired) {
            return stale();
        }
        establish(cookie, now);
        events_.emplace_back(Established{});
        return true;
    }

    const auto local_matches = cookie.local_tag == tcb_->local_tag;
    const auto peer_matches = cookie.peer_tag == tcb_->peer_tag;
    if (opened->expired && !(local_matches && peer_matches)) {
        return stale();
    }
    if (!local_matches && !peer_matches && cookie.tie_tag != 0 && cookie.tie_tag == tcb_->tie_tag) {
        if (state_ == State::ShutdownAckSent) {
            PacketBuilder builder(buffer_, {config_.local_port, config_.remote_port, cookie.peer_tag}, config_.mtu);
            builder.addChunk(ChunkType::ShutdownAck, 0, {});
            builder.addChunk(ChunkType::Error, 0, cause(CauseCode::CookieReceivedWhileShuttingDown));
            listener_.sendPacket(builder.build());
            return false;
        }
        control_.clear();
        establish(cookie, now);
        events_.emplace_back(Restarted{});
        return true;
    }
    if (local_matches && !peer_matches) {
        if (state_ == State::CookieEchoed) {
            establish(cookie, now);
            events_.emplace_back(Established{});
            return true;
        }
        tcb_->peer_tag = cookie.peer_tag;
    }
    else if (!(local_matches && peer_matches)) {
        return false;
    }

    if (state_ == State::CookieEchoed) {
        becomeEstablished(now);
    }
    control_.push_front({.type = ChunkType::CookieAck, .flags = 0, .value = {}});
    return true;
}

void Association::handleData(const Chunk &chunk, std::span<const std::byte> data, std::optional<ByteBuf> &copy,
                             std::vector<Message> &delivered)
{
    if (!tcb_) {
        return;
    }
    const auto header = DataChunk::parse(chunk);
    if (!header) {
        ++dropped_packets_;
        return;
    }
    if (header->payload.empty()) {
        std::vector<std::byte> tsn;
        ByteWriter(tsn).write32(header->tsn);
        fail(CauseCode::NoUserData, CloseReason::ProtocolViolation, tsn);
        return;
    }
    if (header->stream >= tcb_->inbound_streams) {
        tcb_->receiver.acknowledge(*header);
        std::vector<std::byte> stream;
        ByteWriter stream_writer(stream);
        stream_writer.write16(header->stream);
        stream_writer.write16(0);
        queue(ChunkType::Error, 0, cause(CauseCode::InvalidStreamIdentifier, stream));
        return;
    }
    if (!copy) {
        copy = ByteBuf::copyBuffer(data);
    }
    const auto offset = static_cast<std::size_t>(header->payload.data() - data.data());
    tcb_->receiver.receive(*header, copy->retainedSlice(offset, header->payload.size()), delivered);
}

void Association::handleSack(const Chunk &chunk, Clock::time_point now)
{
    if (!tcb_ || state_ == State::CookieEchoed) {
        return;
    }
    const auto sack = SackChunk::parse(chunk);
    if (!sack) {
        ++dropped_packets_;
        return;
    }
    const auto outcome = tcb_->sender.handleSack(*sack, now);
    if (outcome == Sender::SackOutcome::Progressed) {
        tcb_->errors = 0;
    }
    if (outcome != Sender::SackOutcome::Dropped) {
        maybeFinishShutdown(now);
    }
}

void Association::handleHeartbeatAck(const Chunk &chunk, Clock::time_point now)
{
    if (!tcb_ || !tcb_->heartbeat_timeout) {
        return;
    }
    const auto parameters = Tlv::parseAll(chunk.value);
    if (!parameters || parameters->size() != 1 ||
        (*parameters)[0].type != static_cast<std::uint16_t>(ParameterType::HeartbeatInfo) ||
        (*parameters)[0].value.size() != 16) {
        return;
    }
    const auto info = (*parameters)[0].value;
    std::uint64_t sent = 0;
    std::uint64_t nonce = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        sent = sent << 8U | std::to_integer<std::uint64_t>(info[i]);
        nonce = nonce << 8U | std::to_integer<std::uint64_t>(info[8 + i]);
    }
    if (nonce != tcb_->heartbeat_nonce) {
        return;
    }
    const auto sent_at = Clock::time_point{Clock::duration{static_cast<Clock::rep>(sent)}};
    if (sent_at <= now) {
        tcb_->sender.observeRtt(now - sent_at);
    }
    tcb_->heartbeat_timeout.reset();
    tcb_->errors = 0;
}

void Association::handleError(const Chunk &chunk)
{
    const auto causes = Tlv::parseAll(chunk.value);
    if (state_ == State::CookieEchoed && causes && std::ranges::any_of(*causes, [](const Tlv &cause) {
            return cause.type == static_cast<std::uint16_t>(CauseCode::StaleCookie);
        })) {
        close(CloseReason::Timeout);
    }
}

void Association::handleShutdown(Clock::time_point now)
{
    switch (state_) {
    case State::Established:
    case State::ShutdownPending:
        state_ = State::ShutdownReceived;
        maybeFinishShutdown(now);
        break;
    case State::ShutdownSent:
    case State::ShutdownAckSent:
        sendShutdownAck(now);
        break;
    default:
        break;
    }
}

void Association::handleShutdownAck(const Packet &packet)
{
    if (state_ == State::ShutdownSent || state_ == State::ShutdownAckSent) {
        sendStandalone(tcb_->peer_tag, ChunkType::ShutdownComplete, 0, {});
        close(CloseReason::Shutdown);
    }
    else if (state_ == State::CookieWait || state_ == State::CookieEchoed) {
        sendStandalone(packet.header.verification_tag, ChunkType::ShutdownComplete, reflected_tag, {});
    }
}

void Association::handleForwardTsn(const Chunk &chunk, std::vector<Message> &delivered)
{
    if (!tcb_) {
        return;
    }
    if (!tcb_->partial_reliability) {
        fail(CauseCode::ProtocolViolation, CloseReason::ProtocolViolation);
        return;
    }
    const auto forward_tsn = ForwardTsnChunk::parse(chunk);
    if (!forward_tsn) {
        ++dropped_packets_;
        return;
    }
    tcb_->receiver.forwardTsn(*forward_tsn, delivered);
}

void Association::handleReconfig(const Chunk &chunk, Clock::time_point now)
{
    if (!tcb_ || state_ == State::CookieEchoed) {
        return;
    }
    const auto parameters = Tlv::parseAll(chunk.value);
    if (!parameters || parameters->empty() || parameters->size() > 2) {
        queue(ChunkType::Error, 0, cause(CauseCode::ProtocolViolation));
        return;
    }

    std::vector<std::byte> responses;
    ByteWriter writer(responses);
    auto request_seen = false;
    const auto respond = [&](std::uint32_t sequence, ReconfigResult result) {
        std::vector<std::byte> value;
        ByteWriter value_writer(value);
        value_writer.write32(sequence);
        value_writer.write32(static_cast<std::uint32_t>(result));
        writer.writeTlv(static_cast<std::uint16_t>(ParameterType::ReconfigurationResponse), value);
    };
    const auto evaluate = [&](std::uint32_t sequence, const auto &perform) {
        request_seen = true;
        if (sequence == tcb_->last_peer_request + 1) {
            tcb_->last_peer_request = sequence;
            tcb_->last_peer_result = perform();
            respond(sequence, tcb_->last_peer_result);
        }
        else if (sequence == tcb_->last_peer_request) {
            respond(sequence, tcb_->last_peer_result);
        }
        else {
            respond(sequence, ReconfigResult::ErrorBadSequenceNumber);
        }
    };

    for (const auto &parameter : *parameters) {
        switch (static_cast<ParameterType>(parameter.type)) {
        case ParameterType::OutgoingSsnResetRequest: {
            const auto request = OutgoingResetRequest::parse(parameter);
            if (!request) {
                break;
            }
            evaluate(request->request_sequence, [&] {
                if (tcb_->receiver.resetDeferred()) {
                    return ReconfigResult::ErrorRequestAlreadyInProgress;
                }
                if (tcb_->receiver.reached(request->last_assigned_tsn)) {
                    tcb_->receiver.resetStreams(request->streams);
                    events_.emplace_back(IncomingReset{.streams = request->streams});
                    return ReconfigResult::SuccessPerformed;
                }
                tcb_->receiver.deferReset(request->last_assigned_tsn, request->streams);
                tcb_->deferred_request = request->request_sequence;
                return ReconfigResult::InProgress;
            });
            break;
        }
        case ParameterType::IncomingSsnResetRequest:
        case ParameterType::SsnTsnResetRequest:
        case ParameterType::AddOutgoingStreamsRequest:
        case ParameterType::AddIncomingStreamsRequest:
            if (parameter.value.size() >= 4) {
                const auto sequence =
                    static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(parameter.value[0]) << 24U |
                                               std::to_integer<std::uint32_t>(parameter.value[1]) << 16U |
                                               std::to_integer<std::uint32_t>(parameter.value[2]) << 8U |
                                               std::to_integer<std::uint32_t>(parameter.value[3]));
                evaluate(sequence, [] { return ReconfigResult::Denied; });
            }
            break;
        case ParameterType::ReconfigurationResponse: {
            const auto response = ReconfigResponse::parse(parameter);
            if (!response || !tcb_->request || response->response_sequence != tcb_->request->sequence) {
                break;
            }
            if (response->result == ReconfigResult::InProgress) {
                tcb_->request->in_progress = true;
                tcb_->reconfig_backoff = tcb_->sender.rto();
                tcb_->reconfig_at = now + tcb_->reconfig_backoff;
                break;
            }
            const auto performed = response->result == ReconfigResult::SuccessPerformed ||
                                   response->result == ReconfigResult::SuccessNothingToDo;
            auto streams = std::move(tcb_->request->streams);
            tcb_->request.reset();
            tcb_->reconfig_at.reset();
            tcb_->sender.finishReset(streams, performed);
            events_.emplace_back(OutgoingReset{.streams = std::move(streams), .performed = performed});
            break;
        }
        default:
            break;
        }
    }

    if (!responses.empty()) {
        queue(ChunkType::ReConfig, 0, std::move(responses));
    }
    if (request_seen) {
        tcb_->receiver.requireSack();
    }
}

bool Association::handleUnknown(const Chunk &chunk, std::vector<std::byte> &unrecognized)
{
    const auto action = chunk.type >> 6U;
    if ((action == 1 || action == 3) && CommonHeader::size + 2 * Chunk::header_size + padded(unrecognized.size()) +
                                                Tlv::header_size + Chunk::header_size + chunk.value.size() <=
                                            config_.mtu) {
        std::vector<std::byte> original;
        ByteWriter original_writer(original);
        original_writer.write8(chunk.type);
        original_writer.write8(chunk.flags);
        original_writer.write16(static_cast<std::uint16_t>(Chunk::header_size + chunk.value.size()));
        original_writer.write(chunk.value);
        ByteWriter(unrecognized).writeTlv(static_cast<std::uint16_t>(CauseCode::UnrecognizedChunkType), original);
    }
    return action >= 2;
}

void Association::establish(const StateCookie &cookie, Clock::time_point now)
{
    tcb_.reset();
    tcb_.emplace(makeTcb(cookie.local_tag, cookie.local_initial_tsn, cookie.peer_tag, cookie.peer_initial_tsn,
                         cookie.peer_a_rwnd, cookie.peer_outbound_streams, cookie.peer_inbound_streams,
                         {cookie.peer_supports_forward_tsn, cookie.peer_supports_reconfig}));
    state_ = State::Established;
    t1_.reset();
    cookie_echo_.clear();
    if (config_.heartbeat_interval > Clock::duration::zero()) {
        tcb_->heartbeat_at = now + config_.heartbeat_interval + tcb_->sender.rto();
    }
    control_.push_front({.type = ChunkType::CookieAck, .flags = 0, .value = {}});
}

void Association::becomeEstablished(Clock::time_point now)
{
    state_ = State::Established;
    t1_.reset();
    cookie_echo_.clear();
    if (config_.heartbeat_interval > Clock::duration::zero()) {
        tcb_->heartbeat_at = now + config_.heartbeat_interval + tcb_->sender.rto();
    }
    events_.emplace_back(Established{});
}

Association::Tcb Association::makeTcb(std::uint32_t local_tag, std::uint32_t local_initial_tsn, std::uint32_t peer_tag,
                                      std::uint32_t peer_initial_tsn, std::uint32_t peer_window,
                                      std::uint16_t peer_outbound, std::uint16_t peer_inbound,
                                      Capabilities capabilities)
{
    auto tie_tag = random<std::uint64_t>();
    while (tie_tag == 0) {
        tie_tag = random<std::uint64_t>();
    }
    const auto inbound = std::min(config_.streams, peer_outbound);
    return Tcb{
        .local_tag = local_tag,
        .peer_tag = peer_tag,
        .local_initial_tsn = local_initial_tsn,
        .tie_tag = tie_tag,
        .inbound_streams = inbound,
        .outbound_streams = std::min(config_.streams, peer_inbound),
        .partial_reliability = capabilities.partial_reliability,
        .reconfig = capabilities.reconfig,
        .sender = Sender(
            {
                .mtu = config_.mtu,
                .max_buffered = config_.max_send_buffer,
                .rto_initial = config_.rto_initial,
                .rto_min = config_.rto_min,
                .rto_max = config_.rto_max,
                .partial_reliability = capabilities.partial_reliability,
            },
            local_initial_tsn, peer_window),
        .receiver = Receiver(
            {.window = config_.receive_window, .max_message_size = config_.max_message_size, .streams = inbound},
            peer_initial_tsn),
        .next_request_sequence = local_initial_tsn,
        .last_peer_request = peer_initial_tsn - 1,
    };
}

void Association::sendInit()
{
    std::vector<std::byte> parameters;
    ByteWriter writer(parameters);
    writeCapabilities(writer);
    PacketBuilder builder(buffer_, {config_.local_port, config_.remote_port, 0}, config_.mtu);
    builder.addInit(ChunkType::Init,
                    {.initiate_tag = init_tag_,
                     .a_rwnd = static_cast<std::uint32_t>(config_.receive_window),
                     .outbound_streams = config_.streams,
                     .inbound_streams = config_.streams,
                     .initial_tsn = init_tsn_,
                     .parameters = {}},
                    parameters);
    listener_.sendPacket(builder.build());
}

void Association::sendCookieEcho()
{
    sendStandalone(tcb_->peer_tag, ChunkType::CookieEcho, 0, cookie_echo_);
}

void Association::sendStandalone(std::uint32_t verification_tag, ChunkType type, std::uint8_t flags,
                                 std::span<const std::byte> value)
{
    PacketBuilder builder(buffer_, {config_.local_port, config_.remote_port, verification_tag},
                          std::numeric_limits<std::uint16_t>::max());
    builder.addChunk(type, flags, value);
    listener_.sendPacket(builder.build());
}

void Association::queue(ChunkType type, std::uint8_t flags, std::vector<std::byte> value)
{
    control_.push_back({.type = type, .flags = flags, .value = std::move(value)});
}

void Association::queueResetRequest()
{
    std::vector<std::byte> request;
    ByteWriter request_writer(request);
    request_writer.write32(tcb_->request->sequence);
    request_writer.write32(tcb_->last_peer_request);
    request_writer.write32(tcb_->request->last_assigned_tsn);
    for (const auto stream : tcb_->request->streams) {
        request_writer.write16(stream);
    }
    std::vector<std::byte> value;
    ByteWriter(value).writeTlv(static_cast<std::uint16_t>(ParameterType::OutgoingSsnResetRequest), request);
    queue(ChunkType::ReConfig, 0, std::move(value));
}

void Association::transmit(Clock::time_point now)
{
    constexpr unsigned max_burst = 4;
    if (state_ == State::CookieEchoed) {
        control_.clear();
    }
    if (!tcb_ || state_ == State::CookieEchoed) {
        return;
    }
    const auto sends_data =
        state_ == State::Established || state_ == State::ShutdownPending || state_ == State::ShutdownReceived;
    auto &sender = tcb_->sender;
    auto &receiver = tcb_->receiver;
    for (unsigned packet = 0;; ++packet) {
        if (tcb_->reconfig && !tcb_->request) {
            auto streams = sender.takeStreamsReadyToReset();
            if (!streams.empty()) {
                tcb_->request = ResetRequest{
                    .sequence = tcb_->next_request_sequence++,
                    .last_assigned_tsn = sender.lastAssignedTsn(),
                    .streams = std::move(streams),
                };
                tcb_->reconfig_backoff = sender.rto();
                tcb_->reconfig_at = now + tcb_->reconfig_backoff;
                queueResetRequest();
            }
        }

        PacketBuilder builder(buffer_, {config_.local_port, config_.remote_port, tcb_->peer_tag}, config_.mtu);
        while (!control_.empty() &&
               (builder.empty() || Chunk::header_size + padded(control_.front().value.size()) <= builder.remaining())) {
            const auto &front = control_.front();
            builder.addChunk(front.type, front.flags, front.value);
            control_.pop_front();
        }
        if ((receiver.sackRequired() || (receiver.sackPending() && sends_data && sender.hasDataToSend())) &&
            builder.remaining() >= Chunk::header_size + 12) {
            builder.addSack(receiver.makeSack(builder.remaining()));
        }
        if (builder.remaining() >= Chunk::header_size + 4) {
            if (const auto forward_tsn = sender.forwardTsn(now, builder.remaining())) {
                builder.addForwardTsn(*forward_tsn);
            }
        }

        auto data = false;
        if (sends_data && packet < max_burst) {
            data =
                sender.fastRetransmitPending() ? sender.addFastRetransmit(builder, now) : sender.addData(builder, now);
        }
        if (builder.empty()) {
            break;
        }
        listener_.sendPacket(builder.build());
        if (data && tcb_->heartbeat_at) {
            tcb_->heartbeat_at = now + config_.heartbeat_interval + sender.rto();
        }
        if (!data && control_.empty()) {
            break;
        }
    }
}

void Association::maybeFinishShutdown(Clock::time_point now)
{
    if (!tcb_ || !tcb_->sender.idle()) {
        return;
    }
    if (state_ == State::ShutdownPending) {
        sendShutdown(now);
    }
    else if (state_ == State::ShutdownReceived) {
        sendShutdownAck(now);
    }
}

void Association::sendShutdown(Clock::time_point now)
{
    std::vector<std::byte> value;
    ByteWriter(value).write32(tcb_->receiver.cumulativeTsn());
    queue(ChunkType::Shutdown, 0, std::move(value));
    state_ = State::ShutdownSent;
    t2_ = Retry{.at = now + tcb_->sender.rto(), .backoff = tcb_->sender.rto(), .count = t2_ ? t2_->count : 0};
}

void Association::sendShutdownAck(Clock::time_point now)
{
    queue(ChunkType::ShutdownAck, 0, {});
    state_ = State::ShutdownAckSent;
    t2_ = Retry{.at = now + tcb_->sender.rto(), .backoff = tcb_->sender.rto(), .count = t2_ ? t2_->count : 0};
}

void Association::countError()
{
    if (++tcb_->errors > config_.max_retransmissions) {
        sendStandalone(tcb_->peer_tag, ChunkType::Abort, 0, cause(CauseCode::UserInitiatedAbort));
        close(CloseReason::Timeout);
    }
}

void Association::fail(CauseCode code, CloseReason reason, std::span<const std::byte> value)
{
    if (tcb_) {
        sendStandalone(tcb_->peer_tag, ChunkType::Abort, 0, cause(code, value));
    }
    close(reason);
}

void Association::close(CloseReason reason)
{
    if (terminated_) {
        return;
    }
    terminated_ = true;
    state_ = State::Closed;
    tcb_.reset();
    t1_.reset();
    t2_.reset();
    control_.clear();
    cookie_echo_.clear();
    events_.emplace_back(Closed{.reason = reason});
}

void Association::deliver()
{
    if (delivering_) {
        return;
    }
    delivering_ = true;
    while (!events_.empty()) {
        auto event = std::move(events_.front());
        events_.pop_front();
        std::visit(
            [&](auto &&value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, Established>) {
                    listener_.onEstablished();
                }
                else if constexpr (std::is_same_v<T, Restarted>) {
                    listener_.onRestarted();
                }
                else if constexpr (std::is_same_v<T, Closed>) {
                    listener_.onClosed(value.reason);
                }
                else if constexpr (std::is_same_v<T, Message>) {
                    listener_.onMessage(std::move(value));
                }
                else if constexpr (std::is_same_v<T, IncomingReset>) {
                    listener_.onIncomingStreamsReset(value.streams);
                }
                else {
                    listener_.onOutgoingStreamsReset(value.streams, value.performed);
                }
            },
            event);
    }
    delivering_ = false;
}

}  // namespace endgate::rtc::sctp
