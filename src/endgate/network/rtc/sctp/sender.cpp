#include "endgate/network/rtc/sctp/sender.h"

#include <algorithm>
#include <ranges>

#include "endgate/network/rtc/sctp/sequence.h"

namespace endgate::rtc::sctp {
namespace {

std::size_t chunkSize(std::size_t payload)
{
    return DataChunk::header_size + ((payload + 3) & ~std::size_t{3});
}

}  // namespace

Sender::Sender(const Config &config, std::uint32_t initial_tsn, std::uint32_t peer_window)
    : config_(config), cumulative_ack_((std::uint64_t{1} << 32U) + static_cast<std::uint32_t>(initial_tsn - 1)),
      peer_window_(peer_window), cwnd_(10 * config.mtu), ssthresh_(peer_window), srtt_(config.rto_initial),
      rto_(config.rto_initial)
{
}

bool Sender::send(std::uint16_t stream_id, std::uint32_t ppid, ByteBuf payload, const SendOptions &options,
                  Clock::time_point now)
{
    constexpr std::size_t message_overhead = 64;
    if (payload.empty() || buffered_ + message_overhead * (queued_ + 1) + payload.length() > config_.max_buffered) {
        return false;
    }
    auto &stream = streams_[stream_id];
    if (stream.reset != Reset::None) {
        return false;
    }
    const auto length = payload.length();
    stream.queue.push_back({
        .ppid = ppid,
        .payload = std::move(payload),
        .unordered = options.unordered,
        .max_retransmits = options.max_retransmits,
        .expires_at = options.lifetime ? std::optional{now + *options.lifetime} : std::nullopt,
        .id = next_message_++,
    });
    stream.buffered += length;
    buffered_ += length;
    ++queued_;
    ready_.insert(stream_id);
    return true;
}

Sender::SackOutcome Sender::handleSack(const SackChunk &sack, Clock::time_point now)
{
    const auto cumulative = unwrap(sack.cumulative_tsn_ack, cumulative_ack_);
    const auto highest = cumulative_ack_ + outstanding_.size();
    if (cumulative < cumulative_ack_ || cumulative > highest) {
        return SackOutcome::Dropped;
    }

    std::vector<std::pair<std::uint64_t, std::uint64_t>> blocks;
    blocks.reserve(sack.gap_blocks.size());
    for (const auto &block : sack.gap_blocks) {
        if (block.start == 0 || block.start > block.end || cumulative + block.start > highest) {
            continue;
        }
        blocks.emplace_back(cumulative + block.start, std::min<std::uint64_t>(cumulative + block.end, highest));
    }
    std::ranges::sort(blocks);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> acked;
    for (const auto &block : blocks) {
        if (!acked.empty() && block.first <= acked.back().second + 1) {
            acked.back().second = std::max(acked.back().second, block.second);
        }
        else {
            acked.push_back(block);
        }
    }

    const auto flight_before = flight_;
    const auto advanced = cumulative > cumulative_ack_;
    if (advanced && acked.empty()) {
        const auto &newest = outstanding_[cumulative - cumulative_ack_ - 1];
        if (newest.state == State::InFlight && newest.transmissions == 1) {
            observeRtt(now - newest.sent);
        }
    }
    if (fast_recovery_exit_ && cumulative >= *fast_recovery_exit_) {
        fast_recovery_exit_.reset();
    }

    std::size_t bytes_acked = 0;
    std::optional<std::uint64_t> highest_newly_acked;
    while (cumulative_ack_ < cumulative) {
        auto &item = outstanding_.front();
        ++cumulative_ack_;
        if (item.state == State::InFlight || item.state == State::Lost) {
            bytes_acked += chunkSize(item.payload.length());
            highest_newly_acked = cumulative_ack_;
        }
        leaveFlight(item);
        held_ -= item.payload.length();
        outstanding_.pop_front();
    }
    for (const auto &[first, last] : acked) {
        for (auto tsn = std::max(first, cumulative_ack_ + 1); tsn <= last; ++tsn) {
            auto &item = outstanding_[tsn - cumulative_ack_ - 1];
            if (item.state == State::InFlight || item.state == State::Lost) {
                bytes_acked += chunkSize(item.payload.length());
                highest_newly_acked = tsn;
                leaveFlight(item);
                item.state = State::Acked;
            }
        }
    }

    auto loss = false;
    if (highest_newly_acked) {
        const auto limit = fast_recovery_exit_ && advanced && !acked.empty()
                             ? std::max(*highest_newly_acked, acked.back().second)
                             : *highest_newly_acked;
        for (auto tsn = cumulative_ack_ + 1; tsn < limit; ++tsn) {
            const auto index = tsn - cumulative_ack_ - 1;
            auto &item = outstanding_[index];
            if (item.state != State::InFlight || (item.transmissions > 1 && *highest_newly_acked < item.boundary)) {
                continue;
            }
            if (++item.misses < 3 || item.fast_retransmitted) {
                continue;
            }
            loss = true;
            if (exhausted(item, now)) {
                abandon(index);
                continue;
            }
            leaveFlight(item);
            item.state = State::Lost;
            item.fast = true;
            ++lost_;
            fast_retransmit_pending_ = true;
        }
    }

    if (advanced && bytes_acked > 0) {
        if (cwnd_ <= ssthresh_) {
            if (!fast_recovery_exit_ && flight_before + config_.mtu >= cwnd_) {
                cwnd_ += std::min(bytes_acked, config_.mtu);
            }
        }
        else {
            partial_bytes_acked_ += bytes_acked;
            if (partial_bytes_acked_ >= cwnd_ && flight_before + config_.mtu >= cwnd_) {
                partial_bytes_acked_ -= cwnd_;
                cwnd_ += config_.mtu;
            }
        }
    }
    if (loss && !fast_recovery_exit_) {
        ssthresh_ = std::max(cwnd_ / 2, 4 * config_.mtu);
        cwnd_ = ssthresh_;
        partial_bytes_acked_ = 0;
        fast_recovery_exit_ = cumulative_ack_ + outstanding_.size();
    }

    peer_window_ = sack.a_rwnd > flight_payload_ ? sack.a_rwnd - flight_payload_ : 0;
    if (outstanding_.empty()) {
        t3_.reset();
        partial_bytes_acked_ = 0;
    }
    else if (advanced || !t3_) {
        t3_ = now + rto_;
    }
    return advanced || bytes_acked > 0 ? SackOutcome::Progressed : SackOutcome::Stalled;
}

bool Sender::handleTimeout(Clock::time_point now)
{
    if (!t3_ || now < *t3_) {
        return false;
    }
    ssthresh_ = std::max(cwnd_ / 2, 4 * config_.mtu);
    cwnd_ = config_.mtu;
    partial_bytes_acked_ = 0;
    rto_ = std::min(rto_ * 2, config_.rto_max);
    for (std::size_t index = 0; index < outstanding_.size(); ++index) {
        auto &item = outstanding_[index];
        if (item.state != State::InFlight) {
            continue;
        }
        if (exhausted(item, now)) {
            abandon(index);
            continue;
        }
        leaveFlight(item);
        item.state = State::Lost;
        ++lost_;
    }
    forward_tsn_retry_at_ = {};
    t3_ = now + rto_;
    return true;
}

bool Sender::addFastRetransmit(PacketBuilder &builder, Clock::time_point now)
{
    fast_retransmit_pending_ = false;
    auto added = false;
    auto restart = false;
    for (std::size_t index = 0; index < outstanding_.size(); ++index) {
        auto &item = outstanding_[index];
        if (item.state != State::Lost || !item.fast) {
            continue;
        }
        item.fast = false;
        item.fast_retransmitted = true;
        if (exhausted(item, now)) {
            abandon(index);
            continue;
        }
        if (chunkSize(item.payload.length()) > builder.remaining()) {
            continue;
        }
        retransmit(index, builder, now);
        restart = restart || index == 0;
        added = true;
    }
    if (restart || (added && !t3_)) {
        t3_ = now + rto_;
    }
    return added;
}

bool Sender::addData(PacketBuilder &builder, Clock::time_point now)
{
    auto added = false;
    for (std::size_t index = 0; lost_ > 0 && index < outstanding_.size() && flight_ < cwnd_; ++index) {
        auto &item = outstanding_[index];
        if (item.state != State::Lost) {
            continue;
        }
        if (exhausted(item, now)) {
            abandon(index);
            continue;
        }
        if (chunkSize(item.payload.length()) > builder.remaining()) {
            break;
        }
        retransmit(index, builder, now);
        added = true;
    }

    constexpr std::size_t min_fragment = 64;
    const auto whole_packet = (config_.mtu & ~std::size_t{3}) - CommonHeader::size - DataChunk::header_size;
    constexpr std::size_t max_outstanding = 65535;
    while (lost_ == 0 && flight_ < cwnd_ && !ready_.empty() && (peer_window_ > 0 || flight_ == 0) &&
           outstanding_.size() < max_outstanding && held_ < config_.max_buffered &&
           builder.remaining() > DataChunk::header_size) {
        const auto after = ready_.upper_bound(last_stream_);
        const auto stream_id = current_.value_or(after != ready_.end() ? *after : *ready_.begin());
        auto &stream = streams_[stream_id];
        auto &message = stream.queue.front();
        if (message.offset == 0 && message.expires_at && now >= *message.expires_at && config_.partial_reliability) {
            dropFront(stream_id, stream);
            continue;
        }

        const auto remaining = message.payload.length() - message.offset;
        auto fragment = std::min(remaining, builder.remaining() - DataChunk::header_size);
        if (flight_ > 0) {
            fragment = std::min(fragment, peer_window_);
        }
        if (fragment < remaining && (fragment < min_fragment || remaining <= whole_packet)) {
            break;
        }

        if (message.offset == 0) {
            message.ssn = message.unordered ? 0 : stream.next_ssn++;
        }
        const auto flags = static_cast<std::uint8_t>((message.offset == 0 ? DataChunk::beginning_flag : 0) |
                                                     (fragment == remaining ? DataChunk::end_flag : 0) |
                                                     (message.unordered ? DataChunk::unordered_flag : 0) |
                                                     (cwnd_ < 10 * config_.mtu ? DataChunk::immediate_flag : 0));
        auto payload = message.payload.retainedSlice(message.offset, fragment);
        builder.addData({.tsn = static_cast<std::uint32_t>(nextTsn()),
                         .stream = stream_id,
                         .ssn = message.ssn,
                         .ppid = message.ppid,
                         .flags = flags,
                         .payload = {}},
                        payload);
        outstanding_.push_back({
            .stream = stream_id,
            .ssn = message.ssn,
            .ppid = message.ppid,
            .flags = flags,
            .payload = std::move(payload),
            .message = message.id,
            .sent = now,
            .boundary = nextTsn() + 1,
            .max_retransmits = message.max_retransmits,
            .expires_at = message.expires_at,
        });
        flight_ += chunkSize(fragment);
        flight_payload_ += fragment;
        held_ += fragment;
        peer_window_ -= std::min(peer_window_, fragment);
        stream.buffered -= fragment;
        buffered_ -= fragment;
        message.offset += fragment;
        current_ = stream_id;
        if (message.offset == message.payload.length()) {
            stream.queue.pop_front();
            --queued_;
            current_.reset();
            last_stream_ = stream_id;
            if (stream.queue.empty()) {
                ready_.erase(stream_id);
            }
        }
        added = true;
    }
    if (added && !t3_) {
        t3_ = now + rto_;
    }
    return added;
}

std::optional<ForwardTsnChunk> Sender::forwardTsn(Clock::time_point now, std::size_t max_size)
{
    using namespace std::chrono_literals;
    if (!config_.partial_reliability) {
        return std::nullopt;
    }
    const auto capacity = max_size > Chunk::header_size + 4 ? (max_size - Chunk::header_size - 4) / 4 : 0;
    auto advanced = cumulative_ack_;
    auto covered = cumulative_ack_;
    std::vector<ForwardTsnChunk::SkippedStream> skipped;
    for (std::size_t index = 0; index < outstanding_.size(); ++index) {
        const auto &item = outstanding_[index];
        if (item.state == State::Acked && (item.max_retransmits || item.expires_at) &&
            (item.flags & DataChunk::beginning_flag) != 0) {
            auto last = index;
            while (last + 1 < outstanding_.size() && (outstanding_[last].flags & DataChunk::end_flag) == 0 &&
                   outstanding_[last + 1].state == State::Acked && outstanding_[last + 1].message == item.message) {
                ++last;
            }
            if ((outstanding_[last].flags & DataChunk::end_flag) == 0) {
                break;
            }
            advanced += last - index + 1;
            index = last;
            continue;
        }
        if (item.state != State::Abandoned) {
            break;
        }
        if ((item.flags & DataChunk::unordered_flag) == 0) {
            const auto entry = std::ranges::find(skipped, item.stream, &ForwardTsnChunk::SkippedStream::stream);
            if (entry != skipped.end()) {
                entry->ssn = item.ssn;
            }
            else if (skipped.size() < capacity) {
                skipped.push_back({.stream = item.stream, .ssn = item.ssn});
            }
            else {
                break;
            }
        }
        covered = ++advanced;
    }
    advanced = covered;
    if (advanced == cumulative_ack_ || (advanced == last_forward_tsn_ && now < forward_tsn_retry_at_)) {
        return std::nullopt;
    }
    last_forward_tsn_ = advanced;
    forward_tsn_retry_at_ = now + std::min<Clock::duration>(200ms, srtt_);
    return ForwardTsnChunk{.new_cumulative_tsn = static_cast<std::uint32_t>(advanced), .skipped = std::move(skipped)};
}

void Sender::observeRtt(Clock::duration rtt)
{
    using namespace std::chrono_literals;
    constexpr Clock::duration max_rtt = 60s;
    constexpr Clock::duration min_variance = 27500us;
    if (rtt < Clock::duration::zero() || rtt > max_rtt) {
        return;
    }
    if (!measured_) {
        srtt_ = rtt;
        rttvar_ = rtt / 2;
        measured_ = true;
    }
    else {
        rttvar_ = (3 * rttvar_ + (srtt_ > rtt ? srtt_ - rtt : rtt - srtt_)) / 4;
        srtt_ = (7 * srtt_ + rtt) / 8;
    }
    rttvar_ = std::max(rttvar_, min_variance);
    rto_ = std::clamp(srtt_ + 4 * rttvar_, config_.rto_min, config_.rto_max);
}

void Sender::prepareReset(std::uint16_t stream)
{
    auto &outgoing = streams_[stream];
    if (outgoing.reset == Reset::None) {
        outgoing.reset = Reset::Requested;
    }
}

std::vector<std::uint16_t> Sender::takeStreamsReadyToReset()
{
    std::vector<std::uint16_t> ready;
    for (auto &[id, stream] : streams_) {
        if (stream.reset == Reset::Requested && stream.queue.empty()) {
            stream.reset = Reset::InFlight;
            ready.push_back(id);
        }
    }
    return ready;
}

void Sender::finishReset(std::span<const std::uint16_t> streams, bool performed)
{
    for (const auto id : streams) {
        const auto stream = streams_.find(id);
        if (stream == streams_.end() || stream->second.reset != Reset::InFlight) {
            continue;
        }
        if (performed) {
            streams_.erase(stream);
        }
        else {
            stream->second.reset = Reset::None;
        }
    }
}

std::size_t Sender::bufferedAmount(std::uint16_t stream) const
{
    const auto outgoing = streams_.find(stream);
    return outgoing == streams_.end() ? 0 : outgoing->second.buffered;
}

bool Sender::exhausted(const Outstanding &item, Clock::time_point now) const
{
    return config_.partial_reliability && ((item.max_retransmits && item.transmissions > *item.max_retransmits) ||
                                           (item.expires_at && now >= *item.expires_at));
}

void Sender::leaveFlight(Outstanding &item)
{
    if (item.state == State::InFlight) {
        flight_ -= chunkSize(item.payload.length());
        flight_payload_ -= item.payload.length();
    }
    else if (item.state == State::Lost) {
        --lost_;
    }
}

void Sender::retransmit(std::size_t index, PacketBuilder &builder, Clock::time_point now)
{
    auto &item = outstanding_[index];
    builder.addData({.tsn = static_cast<std::uint32_t>(cumulative_ack_ + 1 + index),
                     .stream = item.stream,
                     .ssn = item.ssn,
                     .ppid = item.ppid,
                     .flags = item.flags,
                     .payload = {}},
                    item.payload);
    --lost_;
    item.state = State::InFlight;
    item.sent = now;
    item.boundary = nextTsn();
    item.misses = 0;
    ++item.transmissions;
    flight_ += chunkSize(item.payload.length());
    flight_payload_ += item.payload.length();
}

void Sender::abandon(std::size_t index)
{
    const auto message = outstanding_[index].message;
    const auto stream_id = outstanding_[index].stream;
    auto first = index;
    while (first > 0 && outstanding_[first - 1].message == message) {
        --first;
    }
    for (auto at = first; at < outstanding_.size() && outstanding_[at].message == message; ++at) {
        auto &item = outstanding_[at];
        if (item.state != State::Abandoned) {
            leaveFlight(item);
            item.state = State::Abandoned;
            item.fast = false;
        }
    }
    const auto stream = streams_.find(stream_id);
    if (stream != streams_.end() && !stream->second.queue.empty() && stream->second.queue.front().id == message) {
        dropFront(stream_id, stream->second);
    }
}

void Sender::dropFront(std::uint16_t stream_id, OutgoingStream &stream)
{
    const auto &message = stream.queue.front();
    const auto unsent = message.payload.length() - message.offset;
    stream.buffered -= unsent;
    buffered_ -= unsent;
    stream.queue.pop_front();
    --queued_;
    if (current_ == stream_id) {
        current_.reset();
        last_stream_ = stream_id;
    }
    if (stream.queue.empty()) {
        ready_.erase(stream_id);
    }
}

}  // namespace endgate::rtc::sctp
