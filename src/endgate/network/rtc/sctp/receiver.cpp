#include "endgate/network/rtc/sctp/receiver.h"

#include <algorithm>
#include <limits>

#include "endgate/network/rtc/sctp/sequence.h"

namespace endgate::rtc::sctp {
namespace {

constexpr std::size_t fragment_overhead = 64;
constexpr std::uint64_t ssn_origin = std::uint64_t{1} << 16U;

std::size_t charge(const ByteBuf &payload)
{
    return payload.length() + fragment_overhead;
}

std::size_t charge(const ForwardTsnChunk &chunk)
{
    return fragment_overhead + 4 * chunk.skipped.size();
}

}  // namespace

Receiver::Receiver(const Config &config, std::uint32_t peer_initial_tsn)
    : config_(config), cumulative_((std::uint64_t{1} << 32U) + static_cast<std::uint32_t>(peer_initial_tsn - 1))
{
}

Receiver::Outcome Receiver::receive(const DataChunk &chunk, ByteBuf payload, std::vector<Message> &delivered)
{
    const auto tsn = unwrap(chunk.tsn, cumulative_);
    const auto outcome = track(chunk, tsn);
    if (outcome != Outcome::Accepted) {
        return outcome;
    }
    Fragment fragment{
        .stream = chunk.stream,
        .ssn = chunk.ssn,
        .ppid = chunk.ppid,
        .flags = chunk.flags,
        .payload = std::move(payload),
    };
    if (affectedByReset(tsn, chunk.stream)) {
        buffered_ += charge(fragment.payload);
        held_.emplace_back(std::pair{tsn, std::move(fragment)});
        return Outcome::Accepted;
    }
    assemble(tsn, std::move(fragment), delivered);
    return Outcome::Accepted;
}

Receiver::Outcome Receiver::acknowledge(const DataChunk &chunk)
{
    return track(chunk, unwrap(chunk.tsn, cumulative_));
}

Receiver::Outcome Receiver::track(const DataChunk &chunk, std::uint64_t tsn)
{
    constexpr std::uint64_t max_distance = 65535;
    constexpr std::size_t max_ranges = 4096;
    constexpr std::size_t max_duplicates = 16;

    data_in_packet_ = true;
    if (!seen_data_ || (chunk.flags & DataChunk::immediate_flag) != 0) {
        seen_data_ = true;
        immediate_ = true;
    }

    const auto range =
        std::ranges::lower_bound(received_, tsn, {}, [](const auto &received) { return received.second + 1; });
    if (tsn <= cumulative_ || (range != received_.end() && range->first <= tsn && tsn <= range->second)) {
        if (duplicates_.size() < max_duplicates) {
            duplicates_.push_back(chunk.tsn);
        }
        immediate_ = true;
        return Outcome::Duplicate;
    }
    if (tsn - cumulative_ > max_distance) {
        return Outcome::Dropped;
    }
    if (buffered_ >= config_.window && tsn != cumulative_ + 1) {
        immediate_ = true;
        return Outcome::Dropped;
    }
    if (buffered_ > config_.window + 2 * config_.max_message_size) {
        failure_ = CauseCode::OutOfResource;
        return Outcome::Dropped;
    }

    if (tsn == cumulative_ + 1) {
        cumulative_ = tsn;
        advanceCumulative();
    }
    else if (range != received_.end() && range->second + 1 == tsn) {
        range->second = tsn;
        const auto next = std::next(range);
        if (next != received_.end() && next->first == tsn + 1) {
            range->second = next->second;
            received_.erase(next);
        }
    }
    else if (range != received_.end() && range->first == tsn + 1) {
        range->first = tsn;
    }
    else if (received_.size() < max_ranges) {
        received_.insert(range, {tsn, tsn});
    }
    else {
        immediate_ = true;
        return Outcome::Dropped;
    }
    return Outcome::Accepted;
}

bool Receiver::forwardTsn(const ForwardTsnChunk &chunk, std::vector<Message> &delivered)
{
    data_in_packet_ = true;
    const auto new_cumulative = unwrap(chunk.new_cumulative_tsn, cumulative_);
    if (new_cumulative <= cumulative_) {
        immediate_ = true;
        return false;
    }

    cumulative_ = new_cumulative;
    received_.erase(received_.begin(), std::ranges::find_if(received_, [&](const auto &received) {
                        return received.second > cumulative_;
                    }));
    if (!received_.empty() && received_.front().first <= cumulative_) {
        received_.front().first = cumulative_ + 1;
    }
    advanceCumulative();

    if (deferred_ && new_cumulative > deferred_->last_assigned_tsn) {
        buffered_ += charge(chunk);
        held_.emplace_back(chunk);
        if (buffered_ > config_.window + 2 * config_.max_message_size) {
            failure_ = CauseCode::OutOfResource;
        }
        return true;
    }
    skip(new_cumulative, chunk.skipped, delivered);
    return true;
}

void Receiver::endPacket(Clock::time_point now, Clock::duration delay)
{
    if (!std::exchange(data_in_packet_, false)) {
        return;
    }
    if (!received_.empty() || ++unacknowledged_packets_ >= 2) {
        immediate_ = true;
    }
    if (!immediate_ && !deadline_) {
        deadline_ = now + delay;
    }
}

void Receiver::handleTimeout(Clock::time_point now)
{
    if (deadline_ && now >= *deadline_) {
        immediate_ = true;
        deadline_.reset();
    }
}

SackChunk Receiver::makeSack(std::size_t max_size)
{
    constexpr std::size_t fixed_size = Chunk::header_size + 12;
    SackChunk sack{
        .cumulative_tsn_ack = cumulativeTsn(),
        .a_rwnd = advertisedWindow(),
        .gap_blocks = {},
        .duplicate_tsns = {},
    };
    auto room = max_size > fixed_size ? (max_size - fixed_size) / 4 : 0;
    for (const auto &[first, last] : received_) {
        if (room == 0) {
            break;
        }
        sack.gap_blocks.push_back({
            .start = static_cast<std::uint16_t>(first - cumulative_),
            .end = static_cast<std::uint16_t>(last - cumulative_),
        });
        --room;
    }
    for (const auto tsn : duplicates_) {
        if (room == 0) {
            break;
        }
        sack.duplicate_tsns.push_back(tsn);
        --room;
    }
    duplicates_.clear();
    immediate_ = false;
    deadline_.reset();
    unacknowledged_packets_ = 0;
    return sack;
}

bool Receiver::reached(std::uint32_t last_assigned_tsn) const
{
    return unwrap(last_assigned_tsn, cumulative_) <= cumulative_;
}

void Receiver::resetStreams(std::span<const std::uint16_t> streams)
{
    const auto discard = [&](const OrderedStream &stream) {
        for (const auto &[ssn, message] : stream.blocked) {
            buffered_ -= charge(message.payload);
        }
    };
    if (streams.empty()) {
        for (const auto &[id, stream] : ordered_) {
            discard(stream);
        }
        ordered_.clear();
        return;
    }
    for (const auto id : streams) {
        const auto stream = ordered_.find(id);
        if (stream != ordered_.end()) {
            discard(stream->second);
            ordered_.erase(stream);
        }
    }
}

void Receiver::deferReset(std::uint32_t last_assigned_tsn, std::vector<std::uint16_t> streams)
{
    deferred_ =
        DeferredReset{.last_assigned_tsn = unwrap(last_assigned_tsn, cumulative_), .streams = std::move(streams)};
}

std::optional<std::vector<std::uint16_t>> Receiver::completeDeferredReset(std::vector<Message> &delivered)
{
    if (!deferred_ || deferred_->last_assigned_tsn > cumulative_) {
        return std::nullopt;
    }
    auto streams = std::move(deferred_->streams);
    deferred_.reset();
    resetStreams(streams);
    auto held = std::exchange(held_, {});
    for (auto &item : held) {
        if (auto *data = std::get_if<std::pair<std::uint64_t, Fragment>>(&item)) {
            buffered_ -= charge(data->second.payload);
            assemble(data->first, std::move(data->second), delivered);
            continue;
        }
        const auto &forward = std::get<ForwardTsnChunk>(item);
        buffered_ -= charge(forward);
        skip(unwrap(forward.new_cumulative_tsn, cumulative_), forward.skipped, delivered);
    }
    return streams;
}

std::uint32_t Receiver::advertisedWindow() const
{
    const auto available = config_.window > buffered_ ? config_.window - buffered_ : 0;
    return static_cast<std::uint32_t>(std::min<std::size_t>(available, std::numeric_limits<std::uint32_t>::max()));
}

void Receiver::advanceCumulative()
{
    while (!received_.empty() && received_.front().first == cumulative_ + 1) {
        cumulative_ = received_.front().second;
        received_.erase(received_.begin());
    }
}

void Receiver::assemble(std::uint64_t tsn, Fragment fragment, std::vector<Message> &delivered)
{
    constexpr std::uint8_t whole = DataChunk::beginning_flag | DataChunk::end_flag;
    if ((fragment.flags & whole) == whole) {
        complete(fragment.ssn,
                 {
                     .stream = fragment.stream,
                     .ppid = fragment.ppid,
                     .unordered = (fragment.flags & DataChunk::unordered_flag) != 0,
                     .payload = std::move(fragment.payload),
                 },
                 delivered);
        return;
    }

    buffered_ += charge(fragment.payload);
    const auto inserted = fragments_.emplace(tsn, std::move(fragment)).first;
    const auto same_message = [](const Fragment &left, const Fragment &right) {
        const auto unordered = (left.flags & DataChunk::unordered_flag) != 0;
        return left.stream == right.stream && unordered == ((right.flags & DataChunk::unordered_flag) != 0) &&
               (unordered || left.ssn == right.ssn);
    };

    auto last = inserted;
    while ((last->second.flags & DataChunk::end_flag) == 0) {
        const auto next = std::next(last);
        if (next == fragments_.end() || next->first != last->first + 1 ||
            (next->second.flags & DataChunk::beginning_flag) != 0 || !same_message(last->second, next->second)) {
            return;
        }
        last = next;
    }
    auto first = inserted;
    while ((first->second.flags & DataChunk::beginning_flag) == 0) {
        if (first == fragments_.begin()) {
            return;
        }
        const auto previous = std::prev(first);
        if (previous->first + 1 != first->first || (previous->second.flags & DataChunk::end_flag) != 0 ||
            !same_message(previous->second, first->second)) {
            return;
        }
        first = previous;
    }

    Message message{
        .stream = first->second.stream,
        .ppid = first->second.ppid,
        .unordered = (first->second.flags & DataChunk::unordered_flag) != 0,
        .payload = {},
    };
    const auto ssn = first->second.ssn;
    const auto stop = std::next(last);
    for (auto it = first; it != stop; ++it) {
        buffered_ -= charge(it->second.payload);
        message.payload.appendToChain(std::move(it->second.payload));
    }
    fragments_.erase(first, stop);
    complete(ssn, std::move(message), delivered);
}

void Receiver::complete(std::uint16_t ssn, Message message, std::vector<Message> &delivered)
{
    if (message.payload.length() > config_.max_message_size) {
        failure_ = CauseCode::ProtocolViolation;
        return;
    }
    if (message.unordered) {
        delivered.push_back(std::move(message));
        return;
    }
    auto &stream =
        ordered_.try_emplace(message.stream, OrderedStream{.next_ssn = ssn_origin, .blocked = {}}).first->second;
    const auto unwrapped = unwrap(ssn, stream.next_ssn);
    if (unwrapped < stream.next_ssn) {
        return;
    }
    if (unwrapped != stream.next_ssn) {
        const auto cost = charge(message.payload);
        if (stream.blocked.try_emplace(unwrapped, std::move(message)).second) {
            buffered_ += cost;
        }
        return;
    }
    delivered.push_back(std::move(message));
    ++stream.next_ssn;
    release(stream, delivered);
}

void Receiver::release(OrderedStream &stream, std::vector<Message> &delivered)
{
    while (!stream.blocked.empty() && stream.blocked.begin()->first == stream.next_ssn) {
        auto node = stream.blocked.extract(stream.blocked.begin());
        buffered_ -= charge(node.mapped().payload);
        delivered.push_back(std::move(node.mapped()));
        ++stream.next_ssn;
    }
}

void Receiver::skip(std::uint64_t new_cumulative, std::span<const ForwardTsnChunk::SkippedStream> skipped,
                    std::vector<Message> &delivered)
{
    const auto stop = fragments_.upper_bound(new_cumulative);
    for (auto it = fragments_.begin(); it != stop; ++it) {
        buffered_ -= charge(it->second.payload);
    }
    fragments_.erase(fragments_.begin(), stop);
    for (auto orphan = new_cumulative + 1; !fragments_.empty() && fragments_.begin()->first == orphan &&
                                           (fragments_.begin()->second.flags & DataChunk::beginning_flag) == 0;
         ++orphan) {
        buffered_ -= charge(fragments_.begin()->second.payload);
        fragments_.erase(fragments_.begin());
    }

    for (const auto &entry : skipped) {
        if (entry.stream >= config_.streams) {
            continue;
        }
        auto &stream =
            ordered_.try_emplace(entry.stream, OrderedStream{.next_ssn = ssn_origin, .blocked = {}}).first->second;
        const auto ssn = unwrap(entry.ssn, stream.next_ssn);
        while (!stream.blocked.empty() && stream.blocked.begin()->first <= ssn) {
            auto node = stream.blocked.extract(stream.blocked.begin());
            buffered_ -= charge(node.mapped().payload);
            delivered.push_back(std::move(node.mapped()));
        }
        stream.next_ssn = std::max(stream.next_ssn, ssn + 1);
        release(stream, delivered);
    }
}

bool Receiver::affectedByReset(std::uint64_t tsn, std::uint16_t stream) const
{
    return deferred_ && tsn > deferred_->last_assigned_tsn &&
           (deferred_->streams.empty() || std::ranges::find(deferred_->streams, stream) != deferred_->streams.end());
}

}  // namespace endgate::rtc::sctp
