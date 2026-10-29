#include "endgate/network/raknet/datagram.h"

#include <algorithm>
#include <iterator>
#include <span>

namespace endgate::raknet {

std::expected<DatagramHeader, std::string_view> DatagramHeader::decode(ByteBufView &reader)
{
    if (!reader.isReadable()) {
        return std::unexpected("datagram header is truncated");
    }
    const auto flags = reader.readUnsignedByte();
    if ((flags & 0x80U) == 0) {
        return std::unexpected("dhf.isValid==false");
    }
    DatagramHeader header;
    header.is_ack = (flags & 0x40U) != 0;
    if (header.is_ack) {
        header.has_b_and_as = (flags & 0x20U) != 0;
        if (header.has_b_and_as) {
            if (!reader.isReadable(sizeof(float))) {
                return std::unexpected("datagram header is truncated");
            }
            header.as = reader.readFloat();
        }
        return header;
    }
    header.is_nak = (flags & 0x20U) != 0;
    if (header.is_nak) {
        return header;
    }
    header.is_packet_pair = (flags & 0x10U) != 0;
    header.is_continuous_send = (flags & 0x08U) != 0;
    header.needs_b_and_as = (flags & 0x04U) != 0;
    if (!reader.isReadable(3)) {
        return std::unexpected("datagram header is truncated");
    }
    header.datagram_number = reader.readUnsignedMediumLE();
    return header;
}

void DatagramHeader::encode(ByteBuf &writer) const
{
    if (is_ack) {
        writer.writeByte(has_b_and_as ? 0xE0 : 0xC0);
        if (has_b_and_as) {
            writer.writeFloat(as);
        }
        return;
    }
    if (is_nak) {
        writer.writeByte(0xA0);
        return;
    }
    writer.writeByte(static_cast<std::uint8_t>(0x80U | (is_packet_pair ? 0x10U : 0U) |
                                               (is_continuous_send ? 0x08U : 0U) | (needs_b_and_as ? 0x04U : 0U)));
    writer.writeMediumLE(datagram_number);
}

namespace {

PacketReliability wireReliability(PacketReliability reliability)
{
    switch (reliability) {
    case PacketReliability::UnreliableWithAckReceipt:
        return PacketReliability::Unreliable;
    case PacketReliability::ReliableWithAckReceipt:
        return PacketReliability::Reliable;
    case PacketReliability::ReliableOrderedWithAckReceipt:
        return PacketReliability::ReliableOrdered;
    default:
        return reliability;
    }
}

}  // namespace

std::size_t FrameHeader::length() const
{
    const auto wire = wireReliability(reliability);
    const bool reliable = wire == PacketReliability::Reliable || wire == PacketReliability::ReliableSequenced ||
                          wire == PacketReliability::ReliableOrdered;
    const bool sequenced =
        wire == PacketReliability::UnreliableSequenced || wire == PacketReliability::ReliableSequenced;
    const bool ordered = sequenced || wire == PacketReliability::ReliableOrdered;
    return 1 + 2 + (reliable ? 3 : 0) + (sequenced ? 3 : 0) + (ordered ? 3 + 1 : 0) +
           (split_packet_count > 0 ? 4 + 2 + 4 : 0);
}

void FrameHeader::encode(ByteBuf &writer, std::size_t data_byte_length) const
{
    const auto wire = wireReliability(reliability);
    const bool split = split_packet_count > 0;
    writer.writeByte(static_cast<std::uint8_t>(static_cast<unsigned>(wire) << 5U | (split ? 0x10U : 0U)));
    writer.writeShort(static_cast<std::uint16_t>(data_byte_length * 8));
    if (wire == PacketReliability::Reliable || wire == PacketReliability::ReliableSequenced ||
        wire == PacketReliability::ReliableOrdered) {
        writer.writeMediumLE(reliable_message_number);
    }
    if (wire == PacketReliability::UnreliableSequenced || wire == PacketReliability::ReliableSequenced) {
        writer.writeMediumLE(sequencing_index);
    }
    if (wire == PacketReliability::UnreliableSequenced || wire == PacketReliability::ReliableSequenced ||
        wire == PacketReliability::ReliableOrdered) {
        writer.writeMediumLE(ordering_index);
        writer.writeByte(ordering_channel);
    }
    if (split) {
        writer.writeInt(split_packet_count);
        writer.writeShort(split_packet_id);
        writer.writeInt(split_packet_index);
    }
}

std::expected<Frame, std::string_view> Frame::decode(ByteBufView &reader)
{
    constexpr std::string_view garbage = "frame encoding is garbage";

    if (!reader.isReadable(3)) {
        return std::unexpected(garbage);
    }
    const auto flags = reader.readUnsignedByte();
    const auto bit_length = reader.readUnsignedShort();
    Frame frame;
    auto &header = frame.header;
    header.reliability = static_cast<PacketReliability>(flags >> 5U);
    const bool split = (flags & 0x10U) != 0;
    const auto reliability = header.reliability;
    const bool reliable = reliability == PacketReliability::Reliable ||
                          reliability == PacketReliability::ReliableSequenced ||
                          reliability == PacketReliability::ReliableOrdered;
    const bool sequenced =
        reliability == PacketReliability::UnreliableSequenced || reliability == PacketReliability::ReliableSequenced;
    const bool ordered = sequenced || reliability == PacketReliability::ReliableOrdered ||
                         reliability == PacketReliability::ReliableOrderedWithAckReceipt;
    const std::size_t extra_length =
        (reliable ? 3 : 0) + (sequenced ? 3 : 0) + (ordered ? 3 + 1 : 0) + (split ? 10 : 0);
    if (!reader.isReadable(extra_length)) {
        return std::unexpected(garbage);
    }
    if (reliable) {
        header.reliable_message_number = reader.readUnsignedMediumLE();
    }
    if (sequenced) {
        header.sequencing_index = reader.readUnsignedMediumLE();
    }
    if (ordered) {
        header.ordering_index = reader.readUnsignedMediumLE();
        header.ordering_channel = reader.readUnsignedByte();
    }
    if (split) {
        header.split_packet_count = reader.readUnsignedInt();
        header.split_packet_id = reader.readUnsignedShort();
        header.split_packet_index = reader.readUnsignedInt();
    }
    if (bit_length == 0 || header.ordering_channel >= NUMBER_OF_ORDERED_STREAMS ||
        (split && header.split_packet_index >= header.split_packet_count)) {
        return std::unexpected(garbage);
    }
    const std::size_t data_length = (bit_length + 7U) / 8U;
    if (!reader.isReadable(data_length)) {
        return std::unexpected("frame data is truncated");
    }
    frame.data = reader.readSlice(data_length);
    return frame;
}

void RangeList::insert(std::uint32_t index)
{
    const auto next = std::ranges::lower_bound(ranges_, index, {}, &Range::min);
    if (next != ranges_.begin()) {
        auto &previous = *std::prev(next);
        if (index <= previous.max) {
            return;
        }
        if (index == previous.max + 1) {
            previous.max = index;
            if (next != ranges_.end() && next->min == index + 1) {
                previous.max = next->max;
                ranges_.erase(next);
            }
            return;
        }
    }
    if (next != ranges_.end() && next->min == index) {
        return;
    }
    if (next != ranges_.end() && next->min == index + 1) {
        next->min = index;
        return;
    }
    ranges_.insert(next, Range{.min = index, .max = index});
}

void RangeList::serialize(ByteBuf &writer, std::size_t max_bits)
{
    constexpr std::size_t count_bits = 16;
    constexpr std::size_t index_bits = 32;

    std::size_t bits_written = 0;
    std::size_t count = 0;
    for (const auto &range : ranges_) {
        if (count_bits + bits_written + index_bits * 2 + 1 > max_bits) {
            break;
        }
        bits_written += 8 + index_bits + (range.min == range.max ? 0 : index_bits);
        ++count;
    }
    writer.writeShort(static_cast<std::uint16_t>(count));
    for (const auto &range : std::span{ranges_}.first(count)) {
        writer.writeByte(range.min == range.max ? 1 : 0);
        writer.writeMediumLE(range.min);
        if (range.min != range.max) {
            writer.writeMediumLE(range.max);
        }
    }
    ranges_.erase(ranges_.begin(), ranges_.begin() + static_cast<std::ptrdiff_t>(count));
}

std::expected<std::vector<Range>, RangeListError> RangeList::deserialize(ByteBufView &reader)
{
    if (!reader.isReadable(2)) {
        return std::unexpected(RangeListError::Truncated);
    }
    const auto count = reader.readUnsignedShort();
    constexpr std::size_t smallest_range = 1 + 3;
    if (count > reader.readableBytes() / smallest_range) {
        return std::unexpected(RangeListError::Truncated);
    }
    std::vector<Range> ranges;
    ranges.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (!reader.isReadable(smallest_range)) {
            return std::unexpected(RangeListError::Truncated);
        }
        const auto max_equal_to_min = reader.readUnsignedByte();
        const auto min = reader.readUnsignedMediumLE();
        if (max_equal_to_min != 0) {
            ranges.push_back({.min = min, .max = min});
            continue;
        }
        if (!reader.isReadable(3)) {
            return std::unexpected(RangeListError::Truncated);
        }
        const auto max = reader.readUnsignedMediumLE();
        if (max < min) {
            return std::unexpected(RangeListError::Truncated);
        }
        ranges.push_back({.min = min, .max = max});
    }
    if (ranges.size() > MAX_ACKNOWLEDGEMENT_RANGES_PER_DATAGRAM) {
        return std::unexpected(RangeListError::TooManyRanges);
    }
    return ranges;
}

}  // namespace endgate::raknet
