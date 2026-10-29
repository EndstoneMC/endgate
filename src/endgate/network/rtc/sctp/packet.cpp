#include "endgate/network/rtc/sctp/packet.h"

#include <algorithm>
#include <array>

#include "endgate/network/rtc/sctp/crc32c.h"

namespace endgate::rtc::sctp {
namespace {

std::size_t padded(std::size_t length)
{
    return (length + 3) & ~std::size_t{3};
}

std::optional<std::vector<std::uint16_t>> parseStreams(ByteBufView data)
{
    if (data.readableBytes() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint16_t> streams;
    streams.reserve(data.readableBytes() / 2);
    while (data.isReadable()) {
        streams.push_back(data.readUnsignedShort());
    }
    return streams;
}

}  // namespace

std::optional<Packet> Packet::parse(ByteBufView data)
{
    constexpr std::size_t max_packet_size = 65535;
    if (!data.isReadable(CommonHeader::SIZE + Chunk::HEADER_SIZE) || data.readableBytes() > max_packet_size) {
        return std::nullopt;
    }

    const auto bytes = data.nioBuffer();
    const auto source_port = data.readUnsignedShort();
    const auto destination_port = data.readUnsignedShort();
    const auto verification_tag = data.readUnsignedInt();
    const auto received_checksum = data.readUnsignedIntLE();
    constexpr std::array<std::byte, 4> zero_checksum{};
    auto checksum = crc32c(bytes.first(8));
    checksum = crc32c(zero_checksum, checksum);
    checksum = crc32c(bytes.subspan(CommonHeader::SIZE), checksum);
    if (checksum != received_checksum) {
        return std::nullopt;
    }

    Packet packet{
        .header = {.source_port = source_port,
                   .destination_port = destination_port,
                   .verification_tag = verification_tag},
        .chunks = {},
    };
    while (data.isReadable()) {
        if (!data.isReadable(Chunk::HEADER_SIZE)) {
            return std::nullopt;
        }
        const auto type = data.readUnsignedByte();
        const auto flags = data.readUnsignedByte();
        const std::size_t length = data.readUnsignedShort();
        if (length < Chunk::HEADER_SIZE || length - Chunk::HEADER_SIZE > data.readableBytes()) {
            return std::nullopt;
        }
        packet.chunks.push_back({.type = type, .flags = flags, .value = data.readSlice(length - Chunk::HEADER_SIZE)});
        data.skipBytes(std::min(padded(length) - length, data.readableBytes()));
    }
    return packet;
}

void Packet::seal(ByteBuf &packet)
{
    packet.setIntLE(8, crc32c(packet.nioBuffer()));
}

void CommonHeader::write(ByteBuf &out) const
{
    out.writeShort(source_port).writeShort(destination_port).writeInt(verification_tag).writeInt(0);
}

void Chunk::write(ByteBuf &out) const
{
    const auto length = HEADER_SIZE + value.readableBytes();
    out.writeByte(type)
        .writeByte(flags)
        .writeShort(static_cast<std::uint16_t>(length))
        .writeBytes(value.nioBuffer())
        .writeZero(padded(length) - length);
}

std::optional<std::vector<Tlv>> Tlv::parseAll(ByteBufView data)
{
    std::vector<Tlv> tlvs;
    while (data.isReadable()) {
        if (!data.isReadable(HEADER_SIZE)) {
            return std::nullopt;
        }
        const auto start = data.readerIndex();
        const auto type = data.readUnsignedShort();
        const std::size_t length = data.readUnsignedShort();
        if (length < HEADER_SIZE || length - HEADER_SIZE > data.readableBytes()) {
            return std::nullopt;
        }
        tlvs.push_back({.type = type, .value = data.readSlice(length - HEADER_SIZE), .raw = data.slice(start, length)});
        data.skipBytes(std::min(padded(length) - length, data.readableBytes()));
    }
    return tlvs;
}

void Tlv::write(ByteBuf &out) const
{
    out.writeZero(padded(out.writerIndex()) - out.writerIndex())
        .writeShort(type)
        .writeShort(static_cast<std::uint16_t>(HEADER_SIZE + value.readableBytes()))
        .writeBytes(value.nioBuffer());
}

std::optional<DataChunk> DataChunk::parse(const Chunk &chunk)
{
    constexpr std::size_t fields_size = HEADER_SIZE - Chunk::HEADER_SIZE;
    if (!chunk.value.isReadable(fields_size)) {
        return std::nullopt;
    }
    auto value = chunk.value;
    const auto tsn = value.readUnsignedInt();
    const auto stream = value.readUnsignedShort();
    const auto ssn = value.readUnsignedShort();
    const auto ppid = value.readUnsignedInt();
    return DataChunk{
        .tsn = tsn, .stream = stream, .ssn = ssn, .ppid = ppid, .flags = chunk.flags, .payload = value.slice()};
}

void DataChunk::write(ByteBuf &out) const
{
    const auto length = HEADER_SIZE + payload.readableBytes();
    out.writeByte(static_cast<std::uint8_t>(ChunkType::Data))
        .writeByte(flags)
        .writeShort(static_cast<std::uint16_t>(length))
        .writeInt(tsn)
        .writeShort(stream)
        .writeShort(ssn)
        .writeInt(ppid)
        .writeBytes(payload.nioBuffer())
        .writeZero(padded(length) - length);
}

std::optional<InitChunk> InitChunk::parse(const Chunk &chunk)
{
    constexpr std::size_t fields_size = 16;
    if (!chunk.value.isReadable(fields_size)) {
        return std::nullopt;
    }
    auto value = chunk.value;
    const auto initiate_tag = value.readUnsignedInt();
    const auto a_rwnd = value.readUnsignedInt();
    const auto outbound_streams = value.readUnsignedShort();
    const auto inbound_streams = value.readUnsignedShort();
    const auto initial_tsn = value.readUnsignedInt();
    auto parameters = Tlv::parseAll(value);
    if (!parameters) {
        return std::nullopt;
    }
    return InitChunk{
        .initiate_tag = initiate_tag,
        .a_rwnd = a_rwnd,
        .outbound_streams = outbound_streams,
        .inbound_streams = inbound_streams,
        .initial_tsn = initial_tsn,
        .parameters = std::move(*parameters),
    };
}

void InitChunk::write(ByteBuf &out, ChunkType type, ByteBufView encoded_parameters) const
{
    const auto length = Chunk::HEADER_SIZE + 16 + encoded_parameters.readableBytes();
    out.writeByte(static_cast<std::uint8_t>(type))
        .writeByte(0)
        .writeShort(static_cast<std::uint16_t>(length))
        .writeInt(initiate_tag)
        .writeInt(a_rwnd)
        .writeShort(outbound_streams)
        .writeShort(inbound_streams)
        .writeInt(initial_tsn)
        .writeBytes(encoded_parameters.nioBuffer())
        .writeZero(padded(length) - length);
}

std::optional<SackChunk> SackChunk::parse(const Chunk &chunk)
{
    constexpr std::size_t fields_size = 12;
    if (!chunk.value.isReadable(fields_size)) {
        return std::nullopt;
    }
    auto value = chunk.value;
    SackChunk sack{
        .cumulative_tsn_ack = value.readUnsignedInt(),
        .a_rwnd = value.readUnsignedInt(),
        .gap_blocks = {},
        .duplicate_tsns = {},
    };
    const std::size_t gap_count = value.readUnsignedShort();
    const std::size_t duplicate_count = value.readUnsignedShort();
    if (value.readableBytes() != 4 * (gap_count + duplicate_count)) {
        return std::nullopt;
    }
    sack.gap_blocks.reserve(gap_count);
    for (std::size_t i = 0; i < gap_count; ++i) {
        const auto start = value.readUnsignedShort();
        sack.gap_blocks.push_back({.start = start, .end = value.readUnsignedShort()});
    }
    sack.duplicate_tsns.reserve(duplicate_count);
    for (std::size_t i = 0; i < duplicate_count; ++i) {
        sack.duplicate_tsns.push_back(value.readUnsignedInt());
    }
    return sack;
}

void SackChunk::write(ByteBuf &out) const
{
    out.writeByte(static_cast<std::uint8_t>(ChunkType::Sack))
        .writeByte(0)
        .writeShort(
            static_cast<std::uint16_t>(Chunk::HEADER_SIZE + 12 + 4 * (gap_blocks.size() + duplicate_tsns.size())))
        .writeInt(cumulative_tsn_ack)
        .writeInt(a_rwnd)
        .writeShort(static_cast<std::uint16_t>(gap_blocks.size()))
        .writeShort(static_cast<std::uint16_t>(duplicate_tsns.size()));
    for (const auto &block : gap_blocks) {
        out.writeShort(block.start).writeShort(block.end);
    }
    for (const auto tsn : duplicate_tsns) {
        out.writeInt(tsn);
    }
}

std::optional<ForwardTsnChunk> ForwardTsnChunk::parse(const Chunk &chunk)
{
    if (!chunk.value.isReadable(4) || chunk.value.readableBytes() % 4 != 0) {
        return std::nullopt;
    }
    auto value = chunk.value;
    ForwardTsnChunk forward_tsn{.new_cumulative_tsn = value.readUnsignedInt(), .skipped = {}};
    forward_tsn.skipped.reserve(value.readableBytes() / 4);
    while (value.isReadable()) {
        const auto stream = value.readUnsignedShort();
        forward_tsn.skipped.push_back({.stream = stream, .ssn = value.readUnsignedShort()});
    }
    return forward_tsn;
}

void ForwardTsnChunk::write(ByteBuf &out) const
{
    out.writeByte(static_cast<std::uint8_t>(ChunkType::ForwardTsn))
        .writeByte(0)
        .writeShort(static_cast<std::uint16_t>(Chunk::HEADER_SIZE + 4 + 4 * skipped.size()))
        .writeInt(new_cumulative_tsn);
    for (const auto &entry : skipped) {
        out.writeShort(entry.stream).writeShort(entry.ssn);
    }
}

std::optional<OutgoingResetRequest> OutgoingResetRequest::parse(const Tlv &parameter)
{
    if (!parameter.value.isReadable(12)) {
        return std::nullopt;
    }
    auto value = parameter.value;
    const auto request_sequence = value.readUnsignedInt();
    const auto response_sequence = value.readUnsignedInt();
    const auto last_assigned_tsn = value.readUnsignedInt();
    auto streams = parseStreams(value);
    if (!streams) {
        return std::nullopt;
    }
    return OutgoingResetRequest{
        .request_sequence = request_sequence,
        .response_sequence = response_sequence,
        .last_assigned_tsn = last_assigned_tsn,
        .streams = std::move(*streams),
    };
}

std::optional<IncomingResetRequest> IncomingResetRequest::parse(const Tlv &parameter)
{
    if (!parameter.value.isReadable(4)) {
        return std::nullopt;
    }
    auto value = parameter.value;
    const auto request_sequence = value.readUnsignedInt();
    auto streams = parseStreams(value);
    if (!streams) {
        return std::nullopt;
    }
    return IncomingResetRequest{.request_sequence = request_sequence, .streams = std::move(*streams)};
}

std::optional<ReconfigResponse> ReconfigResponse::parse(const Tlv &parameter)
{
    if (parameter.value.readableBytes() != 8 && parameter.value.readableBytes() != 16) {
        return std::nullopt;
    }
    auto value = parameter.value;
    const auto response_sequence = value.readUnsignedInt();
    const auto result = value.readUnsignedInt();
    if (result > static_cast<std::uint32_t>(ReconfigResult::InProgress)) {
        return std::nullopt;
    }
    return ReconfigResponse{.response_sequence = response_sequence, .result = static_cast<ReconfigResult>(result)};
}

}  // namespace endgate::rtc::sctp
