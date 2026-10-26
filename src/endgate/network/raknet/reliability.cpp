#include "endgate/network/raknet/reliability.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

namespace endgate::raknet {

namespace {

bool isReliable(PacketReliability reliability)
{
    return reliability == PacketReliability::Reliable || reliability == PacketReliability::ReliableSequenced ||
           reliability == PacketReliability::ReliableOrdered ||
           reliability == PacketReliability::ReliableWithAckReceipt ||
           reliability == PacketReliability::ReliableOrderedWithAckReceipt;
}

bool isSequenced(PacketReliability reliability)
{
    return reliability == PacketReliability::UnreliableSequenced || reliability == PacketReliability::ReliableSequenced;
}

/**
 * @see RakNet::ReliabilityLayer::IsOlderOrderedPacket
 */
bool isOlderOrderedPacket(std::uint32_t new_packet_ordering_index, std::uint32_t waiting_for_packet_ordering_index)
{
    constexpr std::uint32_t half_range = UINT24_MASK / 2;

    if (waiting_for_packet_ordering_index > half_range) {
        return new_packet_ordering_index >= waiting_for_packet_ordering_index - half_range + 1 &&
               new_packet_ordering_index < waiting_for_packet_ordering_index;
    }
    return new_packet_ordering_index >= ((waiting_for_packet_ordering_index - (half_range + 1)) & UINT24_MASK) ||
           new_packet_ordering_index < waiting_for_packet_ordering_index;
}

ByteBuf receipt(MessageId id, std::uint32_t serial)
{
    std::array<std::byte, sizeof(MessageId) + sizeof(serial)> bytes{static_cast<std::byte>(id)};
    std::memcpy(bytes.data() + sizeof(MessageId), &serial, sizeof(serial));
    return ByteBuf::copyBuffer(bytes);
}

}  // namespace

bool ReliabilityLayer::HasReceivedPacketQueue::operator[](std::size_t index) const
{
    const auto bit = first_ + index;
    return ((words_[bit / 64] >> (bit % 64)) & 1U) != 0;
}

void ReliabilityLayer::HasReceivedPacketQueue::reset(std::size_t index)
{
    const auto bit = first_ + index;
    words_[bit / 64] &= ~(std::uint64_t{1} << (bit % 64));
}

void ReliabilityLayer::HasReceivedPacketQueue::push(bool hole, std::size_t count)
{
    while (count > 0) {
        const auto bit = first_ + size_;
        if (bit / 64 == words_.size()) {
            if (count >= 64) {
                words_.push_back(hole ? ~std::uint64_t{0} : 0);
                size_ += 64;
                count -= 64;
                continue;
            }
            words_.push_back(0);
        }
        if (hole) {
            words_[bit / 64] |= std::uint64_t{1} << (bit % 64);
        }
        ++size_;
        --count;
    }
}

void ReliabilityLayer::HasReceivedPacketQueue::pop()
{
    ++first_;
    --size_;
    if (first_ == 64) {
        words_.pop_front();
        first_ = 0;
    }
}

ReliabilityLayer::ReliabilityLayer(Listener &listener, std::size_t mtu, TimePoint now)
    : listener_(listener), congestion_manager_(std::clamp(mtu, MINIMUM_MTU_SIZE, MAXIMUM_MTU_SIZE) - UDP_HEADER_SIZE),
      time_last_datagram_arrived_(now), last_update_time_(now)
{
    initHeapWeights();
}

void ReliabilityLayer::handleSocketReceiveFromConnectedPlayer(std::span<const std::byte> datagram, TimePoint now)
{
    last_update_time_ = now;
    if (datagram.size() <= 2) {
        notify("length <= 2 || buffer == 0", true);
        return;
    }
    time_last_datagram_arrived_ = now;

    Reader reader{datagram};
    const auto header = DatagramHeader::decode(reader);
    if (!header) {
        notify(header.error(), true);
        return;
    }
    if (header->is_ack) {
        handleAcknowledgements(reader, now);
        return;
    }
    if (header->is_nak) {
        handleNegativeAcknowledgements(reader, now);
        return;
    }

    const auto skipped = congestion_manager_.onGotPacket(header->datagram_number, now);
    if (!skipped) {
        notify("congestionManager.OnGotPacket failed", true);
        return;
    }
    for (auto offset = *skipped; offset > 0; --offset) {
        naks_.insert((header->datagram_number - offset) & UINT24_MASK);
    }
    remote_system_needs_b_and_as_ = header->needs_b_and_as;
    acknowlegements_.insert(header->datagram_number);

    const auto storage = ByteBuf::copyBuffer(datagram);
    for (bool first = true;; first = false) {
        if (reader.remaining() < Frame::MINIMUM_LENGTH) {
            if (first) {
                notify("CreateInternalPacketFromBitStream failed", true);
            }
            return;
        }
        const auto frame = Frame::decode(reader);
        if (!frame) {
            if (first) {
                notify("CreateInternalPacketFromBitStream failed", true);
            }
            return;
        }
        const auto offset = static_cast<std::size_t>(frame->data.data() - datagram.data());
        handleFrame(frame->header, storage.retainedSlice(offset, frame->data.size()), now);
    }
}

void ReliabilityLayer::handleAcknowledgements(Reader &reader, TimePoint now)
{
    const auto ranges = RangeList::deserialize(reader);
    if (!ranges) {
        notify(ranges.error() == RangeListError::TooManyRanges
                   ? "number of ACK ranges in this datagram exceeds the maximum allowed"
                   : "incomingAcks.Deserialize failed",
               true);
        return;
    }
    std::uint32_t total_acknowledgements = 0;
    for (const auto &range : *ranges) {
        if (range.min > range.max || range.max == UINT24_MASK) {
            notify("incomingAcks minIndex > maxIndex or maxIndex is too large", true);
            return;
        }
        total_acknowledgements += range.max - range.min + 1;
        if (total_acknowledgements > RangeList::MAX_ACKNOWLEDGEMENTS_PER_DATAGRAM) {
            notify("datagram contains too many ACKs, exceeding MAX_TOTAL_ACKS_PER_DATAGRAM", true);
            return;
        }
        for (auto datagram_number = range.min; datagram_number <= range.max; ++datagram_number) {
            for (auto node = unreliable_with_ack_receipt_history_.begin();
                 node != unreliable_with_ack_receipt_history_.end();) {
                if (node->datagram_number != datagram_number) {
                    ++node;
                    continue;
                }
                output_queue_.push_back(receipt(MessageId::SndReceiptAcked, node->send_receipt_serial));
                node = unreliable_with_ack_receipt_history_.erase(node);
            }
            auto *history = getDatagramHistoryNode(datagram_number);
            if (history == nullptr || history->message_numbers.empty()) {
                continue;
            }
            const auto rtt = now > history->time_sent
                               ? std::chrono::duration_cast<CCRakNetSlidingWindow::Duration>(now - history->time_sent)
                               : CCRakNetSlidingWindow::Duration{0};
            congestion_manager_.onAck(rtt, bandwidth_exceeded_statistic_, datagram_number);
            for (const auto message_number : history->message_numbers) {
                removePacketFromResendList(message_number);
            }
            history->message_numbers.clear();
        }
    }
}

void ReliabilityLayer::handleNegativeAcknowledgements(Reader &reader, TimePoint now)
{
    const auto ranges = RangeList::deserialize(reader);
    if (!ranges) {
        notify(ranges.error() == RangeListError::TooManyRanges
                   ? "number of NAK ranges in this datagram exceeds the maximum allowed"
                   : "incomingNAKs.Deserialize failed",
               true);
        return;
    }
    std::uint32_t total_acknowledgements = 0;
    auto resend_position = resend_list_.begin();
    for (const auto &range : *ranges) {
        if (range.min > range.max || range.max == UINT24_MASK) {
            notify("incomingNAKs minIndex>maxIndex or maxIndex is too large", true);
            return;
        }
        total_acknowledgements += range.max - range.min + 1;
        if (total_acknowledgements > RangeList::MAX_ACKNOWLEDGEMENTS_PER_DATAGRAM) {
            notify("datagram contains too many NAKs, exceeding MAX_TOTAL_NAKS_PER_DATAGRAM", true);
            return;
        }
        for (auto datagram_number = range.min; datagram_number <= range.max; ++datagram_number) {
            congestion_manager_.onNak();
            const auto *history = getDatagramHistoryNode(datagram_number);
            if (history == nullptr) {
                continue;
            }
            for (const auto message_number : history->message_numbers) {
                const auto &slot = resend_buffer_[message_number % RESEND_BUFFER_ARRAY_LENGTH];
                if (!slot || (*slot)->reliable_message_number != message_number) {
                    continue;
                }
                (*slot)->next_action_time = now;
                if (*slot == resend_position) {
                    ++resend_position;
                    continue;
                }
                resend_list_.splice(resend_position, resend_list_, *slot);
            }
        }
    }
}

void ReliabilityLayer::handleFrame(const FrameHeader &header, ByteBuf data, TimePoint now)
{
    const auto reliability = header.reliability;
    if (reliability == PacketReliability::Reliable || reliability == PacketReliability::ReliableSequenced ||
        reliability == PacketReliability::ReliableOrdered) {
        const auto hole_count = (header.reliable_message_number - received_packets_base_index_) & UINT24_MASK;
        if (hole_count == 0) {
            if (!has_received_packet_queue_.empty()) {
                has_received_packet_queue_.pop();
            }
            received_packets_base_index_ = (received_packets_base_index_ + 1) & UINT24_MASK;
        }
        else if (hole_count > UINT24_MASK / 2) {
            notify("holeCount > typeRange/(DatagramSequenceNumberType) 2", false);
            return;
        }
        else if (hole_count < has_received_packet_queue_.size()) {
            if (!has_received_packet_queue_[hole_count]) {
                return;
            }
            has_received_packet_queue_.reset(hole_count);
        }
        else {
            if (hole_count > MAXIMUM_HOLE_COUNT) {
                notify("holeCount > 1000000", true);
                return;
            }
            has_received_packet_queue_.push(true, hole_count - has_received_packet_queue_.size());
            has_received_packet_queue_.push(false, 1);
        }
        while (!has_received_packet_queue_.empty() && !has_received_packet_queue_[0]) {
            has_received_packet_queue_.pop();
            received_packets_base_index_ = (received_packets_base_index_ + 1) & UINT24_MASK;
        }
    }

    if (header.split_packet_count > 0) {
        auto assembled = insertIntoSplitPacketList(header, std::move(data), now);
        if (assembled) {
            deliver(*std::move(assembled));
        }
        return;
    }

    InternalPacket packet;
    static_cast<FrameHeader &>(packet) = header;
    packet.data = std::move(data);
    packet.creation_time = now;
    deliver(std::move(packet));
}

std::optional<InternalPacket> ReliabilityLayer::insertIntoSplitPacketList(const FrameHeader &header, ByteBuf data,
                                                                          TimePoint now)
{
    if (header.split_packet_count > MAX_SPLIT_PACKET_COUNT) {
        notify("splitPacketCount exceeds MAX_SPLIT_PACKET_COUNT", true);
        return std::nullopt;
    }
    auto channel = split_packet_channels_.find(header.split_packet_id);
    if (channel == split_packet_channels_.end()) {
        if (split_packet_channels_.size() >= MAX_SPLIT_PACKET_CHANNELS) {
            notify("split packet channels exceed MAX_SPLIT_PACKET_CHANNELS", true);
            return std::nullopt;
        }
        channel = split_packet_channels_
                      .emplace(header.split_packet_id, SplitPacketChannel{header.split_packet_count, 0, header, {}})
                      .first;
    }
    auto &split = channel->second;
    if (header.split_packet_count != split.split_packet_count) {
        notify("splitPacketCount differs within one splitPacketId", true);
        return std::nullopt;
    }
    if (split_packet_bytes_ + data.length() > MAX_SPLIT_PACKET_BYTES) {
        notify("split packets exceed MAX_SPLIT_PACKET_BYTES", true);
        return std::nullopt;
    }
    auto position = split.parts.end();
    if (!split.parts.empty() && header.split_packet_index <= split.parts.back().first) {
        position = std::ranges::lower_bound(split.parts, header.split_packet_index, {},
                                            &std::pair<std::uint32_t, ByteBuf>::first);
        if (position->first == header.split_packet_index) {
            return std::nullopt;
        }
    }
    split.bytes += data.length();
    split_packet_bytes_ += data.length();
    split.parts.emplace(position, header.split_packet_index, std::move(data));
    if (split.parts.size() < split.split_packet_count) {
        return std::nullopt;
    }

    sendAcks();
    InternalPacket packet;
    static_cast<FrameHeader &>(packet) = split.first_header;
    packet.split_packet_count = 0;
    packet.creation_time = now;
    for (auto &[index, part] : split.parts) {
        packet.data.appendToChain(std::move(part));
    }
    split_packet_bytes_ -= split.bytes;
    split_packet_channels_.erase(channel);
    return packet;
}

void ReliabilityLayer::deliver(InternalPacket packet)
{
    const auto reliability = packet.reliability;
    const bool sequenced = isSequenced(reliability);
    if (!sequenced && reliability != PacketReliability::ReliableOrdered) {
        output_queue_.push_back(std::move(packet.data));
        return;
    }

    const auto channel = packet.ordering_channel;
    auto &heap = ordering_heaps_[channel];
    constexpr auto lighter = [](const OrderedPacket &a, const OrderedPacket &b) {
        return a.weight > b.weight;
    };
    if (packet.ordering_index == ordered_read_index_[channel]) {
        if (sequenced) {
            if (isOlderOrderedPacket(packet.sequencing_index, highest_sequenced_read_index_[channel])) {
                return;
            }
            highest_sequenced_read_index_[channel] = (packet.sequencing_index + 1) & UINT24_MASK;
            output_queue_.push_back(std::move(packet.data));
            return;
        }
        output_queue_.push_back(std::move(packet.data));
        ordered_read_index_[channel] = (ordered_read_index_[channel] + 1) & UINT24_MASK;
        highest_sequenced_read_index_[channel] = 0;
        while (!heap.empty() && heap.front().packet.ordering_index == ordered_read_index_[channel]) {
            std::ranges::pop_heap(heap, lighter);
            auto next = std::move(heap.back().packet);
            heap.pop_back();
            if (next.reliability == PacketReliability::ReliableOrdered) {
                ordered_read_index_[channel] = (ordered_read_index_[channel] + 1) & UINT24_MASK;
            }
            else {
                highest_sequenced_read_index_[channel] = next.sequencing_index;
            }
            output_queue_.push_back(std::move(next.data));
        }
        return;
    }
    if (isOlderOrderedPacket(packet.ordering_index, ordered_read_index_[channel])) {
        return;
    }
    if (heap.size() >= MAXIMUM_ORDERED_PACKETS_PER_HEAP) {
        notify("ordering heap exceeds MAXIMUM_ORDERED_PACKETS_PER_HEAP", true);
        return;
    }
    if (heap.empty()) {
        heap_index_offsets_[channel] = ordered_read_index_[channel];
    }
    constexpr std::uint64_t ordered_slot = 1048576;
    const std::uint64_t ordered_hole_count = (packet.ordering_index - heap_index_offsets_[channel]) & UINT24_MASK;
    const auto weight = ordered_hole_count * ordered_slot + (sequenced ? packet.sequencing_index : ordered_slot - 1);
    heap.push_back({weight, std::move(packet)});
    std::ranges::push_heap(heap, lighter);
}

std::optional<ByteBuf> ReliabilityLayer::receive()
{
    if (output_queue_.empty()) {
        return std::nullopt;
    }
    auto message = std::move(output_queue_.front());
    output_queue_.pop_front();
    return message;
}

void ReliabilityLayer::removePacketFromResendList(std::uint32_t message_number)
{
    auto &slot = resend_buffer_[message_number % RESEND_BUFFER_ARRAY_LENGTH];
    if (!slot || (*slot)->reliable_message_number != message_number) {
        return;
    }
    const auto packet = *slot;
    slot.reset();
    unacknowledged_bytes_ -= packet->header_length + packet->data.length();
    if ((packet->reliability == PacketReliability::ReliableWithAckReceipt ||
         packet->reliability == PacketReliability::ReliableOrderedWithAckReceipt) &&
        (packet->split_packet_count == 0 || packet->split_packet_index + 1 == packet->split_packet_count)) {
        output_queue_.push_back(receipt(MessageId::SndReceiptAcked, packet->send_receipt_serial));
    }
    resend_list_.erase(packet);
}

bool ReliabilityLayer::send(ByteBuf data, PacketPriority priority, PacketReliability reliability,
                            std::uint8_t ordering_channel, std::uint32_t receipt, TimePoint now)
{
    last_update_time_ = now;
    if (data.empty()) {
        return false;
    }
    if (ordering_channel >= NUMBER_OF_ORDERED_STREAMS) {
        ordering_channel = 0;
    }

    InternalPacket packet;
    packet.creation_time = now;
    packet.priority = priority;
    packet.reliability = reliability;
    packet.send_receipt_serial = receipt;
    packet.data = std::move(data);

    const auto max_data_size = getMaxDatagramSizeExcludingMessageHeaderBytes() - MAXIMUM_FRAME_HEADER_LENGTH;
    const bool split = packet.data.length() > max_data_size;
    if (split) {
        if (packet.reliability == PacketReliability::Unreliable) {
            packet.reliability = PacketReliability::Reliable;
        }
        else if (packet.reliability == PacketReliability::UnreliableWithAckReceipt) {
            packet.reliability = PacketReliability::ReliableWithAckReceipt;
        }
        else if (packet.reliability == PacketReliability::UnreliableSequenced) {
            packet.reliability = PacketReliability::ReliableSequenced;
        }
    }

    if (isSequenced(packet.reliability)) {
        packet.ordering_channel = ordering_channel;
        packet.ordering_index = ordered_write_index_[ordering_channel];
        packet.sequencing_index = sequenced_write_index_[ordering_channel];
        sequenced_write_index_[ordering_channel] = (sequenced_write_index_[ordering_channel] + 1) & UINT24_MASK;
    }
    else if (packet.reliability == PacketReliability::ReliableOrdered ||
             packet.reliability == PacketReliability::ReliableOrderedWithAckReceipt) {
        packet.ordering_channel = ordering_channel;
        packet.ordering_index = ordered_write_index_[ordering_channel];
        ordered_write_index_[ordering_channel] = (ordered_write_index_[ordering_channel] + 1) & UINT24_MASK;
        sequenced_write_index_[ordering_channel] = 0;
    }

    if (split) {
        splitPacket(std::move(packet));
        return true;
    }
    pushOutgoing(std::move(packet));
    return true;
}

void ReliabilityLayer::splitPacket(InternalPacket packet)
{
    const auto max_data_size = getMaxDatagramSizeExcludingMessageHeaderBytes() - MAXIMUM_FRAME_HEADER_LENGTH;
    const auto data = std::move(packet.data);
    const auto length = data.length();
    packet.split_packet_count = static_cast<std::uint32_t>((length - 1) / max_data_size + 1);
    packet.split_packet_id = split_packet_id_++;
    packet.header_length = packet.length();
    for (std::uint32_t index = 0; index < packet.split_packet_count; ++index) {
        const auto offset = index * max_data_size;
        auto part = packet;
        part.split_packet_index = index;
        part.data = data.retainedSlice(offset, std::min(max_data_size, length - offset));
        pushOutgoing(std::move(part));
    }
}

void ReliabilityLayer::pushOutgoing(InternalPacket packet)
{
    const auto weight = getNextWeight(packet.priority);
    outgoing_packet_buffer_[static_cast<std::size_t>(packet.priority)].push_back({weight, std::move(packet)});
}

std::deque<ReliabilityLayer::OutgoingPacket> *ReliabilityLayer::peekOutgoing()
{
    std::deque<OutgoingPacket> *lightest = nullptr;
    for (auto &queue : outgoing_packet_buffer_) {
        if (!queue.empty() && (lightest == nullptr || queue.front().weight < lightest->front().weight)) {
            lightest = &queue;
        }
    }
    return lightest;
}

void ReliabilityLayer::initHeapWeights()
{
    for (std::uint64_t priority = 0; priority < NUMBER_OF_PRIORITIES; ++priority) {
        outgoing_packet_buffer_next_weights_[priority] = (std::uint64_t{1} << priority) * priority + priority;
    }
}

std::uint64_t ReliabilityLayer::getNextWeight(PacketPriority priority)
{
    const auto level = static_cast<std::uint64_t>(priority);
    auto next = outgoing_packet_buffer_next_weights_[level];
    const auto *head = peekOutgoing();
    if (head == nullptr) {
        initHeapWeights();
        return next;
    }
    const auto peek_level = static_cast<std::uint64_t>(head->front().packet.priority);
    const auto min = head->front().weight - (std::uint64_t{1} << peek_level) * peek_level + peek_level;
    if (next < min) {
        next = min + (std::uint64_t{1} << level) * level + level;
    }
    outgoing_packet_buffer_next_weights_[level] = next + (std::uint64_t{1} << level) * (level + 1) + level;
    return next;
}

std::size_t ReliabilityLayer::getMaxDatagramSizeExcludingMessageHeaderBytes() const
{
    return congestion_manager_.getMtu() - DatagramHeader::DATA_HEADER_BYTE_LENGTH;
}

bool ReliabilityLayer::isOutgoingDataWaiting() const
{
    return !resend_list_.empty() ||
           std::ranges::any_of(outgoing_packet_buffer_, [](const auto &queue) { return !queue.empty(); });
}

bool ReliabilityLayer::ackTimeout(TimePoint now) const
{
    return now - time_last_datagram_arrived_ >= timeout_time_;
}

bool ReliabilityLayer::resendBufferOverflow() const
{
    return resend_buffer_[send_reliable_message_number_index_ % RESEND_BUFFER_ARRAY_LENGTH].has_value();
}

ReliabilityLayer::DatagramHistoryNode *ReliabilityLayer::getDatagramHistoryNode(std::uint32_t datagram_number)
{
    if (datagram_history_.empty() || CCRakNetSlidingWindow::lessThan(datagram_number, datagram_history_pop_count_)) {
        return nullptr;
    }
    const auto offset = (datagram_number - datagram_history_pop_count_) & UINT24_MASK;
    if (offset >= datagram_history_.size()) {
        return nullptr;
    }
    return &datagram_history_[offset];
}

void ReliabilityLayer::sendAcks()
{
    const auto max_bits = getMaxDatagramSizeExcludingMessageHeaderBytes() * 8;
    while (!acknowlegements_.empty()) {
        Writer writer{update_buffer_};
        DatagramHeader{.is_ack = true}.encode(writer);
        acknowlegements_.serialize(writer, max_bits);
        listener_.sendDatagram(writer.written());
        congestion_manager_.onSendAck();
    }
}

void ReliabilityLayer::pushPacket(const InternalPacket &packet)
{
    const auto bytes = packet.header_length + packet.data.length();
    datagram_size_so_far_ += bytes;
    all_datagram_sizes_so_far_ += bytes;
    datagram_packets_.push_back(&packet);
}

void ReliabilityLayer::pushDatagram(TimePoint now)
{
    if (datagram_packets_.empty()) {
        return;
    }
    auto header = datagram_header_;
    header.is_continuous_send = header.is_continuous_send || datagrams_this_update_ > 0;
    header.datagram_number = congestion_manager_.getAndIncrementNextDatagramSequenceNumber();

    Writer writer{update_buffer_};
    header.encode(writer);
    DatagramHistoryNode history{{}, now};
    for (const auto *packet : datagram_packets_) {
        if (isReliable(packet->reliability)) {
            history.message_numbers.push_back(packet->reliable_message_number);
        }
        packet->encode(writer, packet->data.length());
        packet->data.copyTo(writer.claim(packet->data.length()));
    }
    if (datagram_history_.size() > DATAGRAM_MESSAGE_ID_ARRAY_LENGTH) {
        datagram_history_.pop_front();
        datagram_history_pop_count_ = (datagram_history_pop_count_ + 1) & UINT24_MASK;
    }
    datagram_history_.push_back(std::move(history));

    listener_.sendDatagram(writer.written());
    datagram_packets_.clear();
    datagram_size_so_far_ = 0;
    ++datagrams_this_update_;
}

void ReliabilityLayer::update(TimePoint now)
{
    last_update_time_ = now;
    if (dead_connection_) {
        return;
    }
    if (!resend_list_.empty() && ackTimeout(now)) {
        dead_connection_ = true;
        return;
    }

    if (congestion_manager_.shouldSendAcks(now)) {
        sendAcks();
    }
    const auto max_bytes = getMaxDatagramSizeExcludingMessageHeaderBytes();
    if (!naks_.empty()) {
        Writer writer{update_buffer_};
        DatagramHeader{.is_nak = true}.encode(writer);
        naks_.serialize(writer, max_bytes * 8);
        listener_.sendDatagram(writer.written());
    }

    datagram_header_ = DatagramHeader{.is_continuous_send = bandwidth_exceeded_statistic_,
                                      .needs_b_and_as = congestion_manager_.isInSlowStart()};
    bandwidth_exceeded_statistic_ = peekOutgoing() != nullptr;
    const bool has_data_to_send_or_resend = !resend_list_.empty() || bandwidth_exceeded_statistic_;

    for (auto node = unreliable_with_ack_receipt_history_.begin();
         node != unreliable_with_ack_receipt_history_.end();) {
        if (now < node->next_action_time) {
            ++node;
            continue;
        }
        output_queue_.push_back(receipt(MessageId::SndReceiptLoss, node->send_receipt_serial));
        node = unreliable_with_ack_receipt_history_.erase(node);
    }

    if (!has_data_to_send_or_resend) {
        return;
    }

    datagram_packets_.clear();
    unreliable_packets_this_update_.clear();
    datagram_size_so_far_ = 0;
    datagrams_this_update_ = 0;
    const auto transmission_bandwidth =
        congestion_manager_.getTransmissionBandwidth(unacknowledged_bytes_, datagram_header_.is_continuous_send);
    const auto retransmission_bandwidth = congestion_manager_.getRetransmissionBandwidth(unacknowledged_bytes_);

    all_datagram_sizes_so_far_ = 0;
    if (retransmission_bandwidth > 0 || transmission_bandwidth > 0) {
        while (all_datagram_sizes_so_far_ < retransmission_bandwidth) {
            bool pushed_anything = false;
            while (!resend_list_.empty()) {
                auto &packet = resend_list_.front();
                if (now < packet.next_action_time ||
                    datagram_size_so_far_ + packet.header_length + packet.data.length() > max_bytes) {
                    pushDatagram(now);
                    break;
                }
                resend_list_.splice(resend_list_.end(), resend_list_, resend_list_.begin());
                pushPacket(packet);
                ++packet.times_sent;
                congestion_manager_.onResend();
                packet.next_action_time = now + congestion_manager_.getRtoForRetransmission();
                pushed_anything = true;
            }
            if (!pushed_anything) {
                break;
            }
        }
    }

    if (all_datagram_sizes_so_far_ < transmission_bandwidth) {
        all_datagram_sizes_so_far_ = 0;
        while (!resendBufferOverflow() && all_datagram_sizes_so_far_ < transmission_bandwidth) {
            while (auto *queue = peekOutgoing()) {
                auto &head = queue->front().packet;
                if (!isReliable(head.reliability) && unreliable_timeout_.count() > 0 &&
                    now > head.creation_time + unreliable_timeout_) {
                    queue->pop_front();
                    continue;
                }
                head.header_length = head.length();
                if (datagram_size_so_far_ + head.header_length + head.data.length() > max_bytes) {
                    break;
                }
                auto packet = std::move(head);
                queue->pop_front();
                ++packet.times_sent;
                if (isReliable(packet.reliability)) {
                    const auto number = send_reliable_message_number_index_;
                    packet.reliable_message_number = number;
                    packet.next_action_time = now + congestion_manager_.getRtoForRetransmission();
                    unacknowledged_bytes_ += packet.header_length + packet.data.length();
                    resend_list_.push_back(std::move(packet));
                    resend_buffer_[number % RESEND_BUFFER_ARRAY_LENGTH] = std::prev(resend_list_.end());
                    send_reliable_message_number_index_ = (number + 1) & UINT24_MASK;
                    pushPacket(resend_list_.back());
                }
                else {
                    if (packet.reliability == PacketReliability::UnreliableWithAckReceipt) {
                        unreliable_with_ack_receipt_history_.push_back(
                            {congestion_manager_.getNextDatagramSequenceNumber(), packet.send_receipt_serial,
                             now + congestion_manager_.getRtoForRetransmission()});
                    }
                    unreliable_packets_this_update_.push_back(std::move(packet));
                    pushPacket(unreliable_packets_this_update_.back());
                }
                if (resendBufferOverflow()) {
                    break;
                }
            }
            if (datagram_size_so_far_ == 0) {
                break;
            }
            pushDatagram(now);
        }
    }
    pushDatagram(now);
    unreliable_packets_this_update_.clear();
    bandwidth_exceeded_statistic_ = peekOutgoing() != nullptr;
}

std::optional<TimePoint> ReliabilityLayer::nextTimeout() const
{
    if (dead_connection_) {
        return std::nullopt;
    }
    std::optional<TimePoint> earliest;
    const auto consider = [&earliest](TimePoint candidate) {
        if (!earliest || candidate < *earliest) {
            earliest = candidate;
        }
    };
    if (!resend_list_.empty()) {
        consider(resend_list_.front().next_action_time);
        consider(time_last_datagram_arrived_ + timeout_time_);
    }
    if (!acknowlegements_.empty()) {
        consider(congestion_manager_.getAckDeadline().value_or(last_update_time_));
    }
    if (!naks_.empty()) {
        consider(last_update_time_);
    }
    for (const auto &node : unreliable_with_ack_receipt_history_) {
        consider(node.next_action_time);
    }
    return earliest;
}

}  // namespace endgate::raknet
