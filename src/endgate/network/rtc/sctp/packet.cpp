#include "endgate/network/rtc/sctp/packet.h"

#include <algorithm>
#include <array>

#include "endgate/network/rtc/sctp/crc32c.h"

namespace endgate::rtc::sctp {
namespace {

std::uint16_t load16(std::span<const std::byte> data, std::size_t offset)
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(data[offset]) << 8U |
                                      std::to_integer<std::uint16_t>(data[offset + 1]));
}

std::uint32_t load32(std::span<const std::byte> data, std::size_t offset)
{
    return static_cast<std::uint32_t>(load16(data, offset)) << 16U | load16(data, offset + 2);
}

std::size_t padded(std::size_t length)
{
    return (length + 3) & ~std::size_t{3};
}

std::optional<std::vector<std::uint16_t>> parseStreams(std::span<const std::byte> data)
{
    if (data.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint16_t> streams;
    streams.reserve(data.size() / 2);
    for (std::size_t offset = 0; offset < data.size(); offset += 2) {
        streams.push_back(load16(data, offset));
    }
    return streams;
}

}  // namespace

std::optional<Packet> Packet::parse(std::span<const std::byte> data)
{
    constexpr std::size_t max_packet_size = 65535;
    if (data.size() < CommonHeader::Size + Chunk::HeaderSize || data.size() > max_packet_size) {
        return std::nullopt;
    }

    const auto received_checksum =
        std::to_integer<std::uint32_t>(data[8]) | std::to_integer<std::uint32_t>(data[9]) << 8U |
        std::to_integer<std::uint32_t>(data[10]) << 16U | std::to_integer<std::uint32_t>(data[11]) << 24U;
    constexpr std::array<std::byte, 4> zero_checksum{};
    auto checksum = crc32c(data.first(8));
    checksum = crc32c(zero_checksum, checksum);
    checksum = crc32c(data.subspan(CommonHeader::Size), checksum);
    if (checksum != received_checksum) {
        return std::nullopt;
    }

    Packet packet{
        .header = {.source_port = load16(data, 0),
                   .destination_port = load16(data, 2),
                   .verification_tag = load32(data, 4)},
        .chunks = {},
    };
    auto remaining = data.subspan(CommonHeader::Size);
    while (!remaining.empty()) {
        if (remaining.size() < Chunk::HeaderSize) {
            return std::nullopt;
        }
        const auto length = load16(remaining, 2);
        if (length < Chunk::HeaderSize || length > remaining.size()) {
            return std::nullopt;
        }
        packet.chunks.push_back({
            .type = std::to_integer<std::uint8_t>(remaining[0]),
            .flags = std::to_integer<std::uint8_t>(remaining[1]),
            .value = remaining.subspan(Chunk::HeaderSize, length - Chunk::HeaderSize),
        });
        remaining = remaining.subspan(std::min(padded(length), remaining.size()));
    }
    return packet;
}

std::optional<std::vector<Tlv>> Tlv::parseAll(std::span<const std::byte> data)
{
    std::vector<Tlv> tlvs;
    while (!data.empty()) {
        if (data.size() < HeaderSize) {
            return std::nullopt;
        }
        const auto length = load16(data, 2);
        if (length < HeaderSize || length > data.size()) {
            return std::nullopt;
        }
        tlvs.push_back({
            .type = load16(data, 0),
            .value = data.subspan(HeaderSize, length - HeaderSize),
            .raw = data.first(length),
        });
        data = data.subspan(std::min(padded(length), data.size()));
    }
    return tlvs;
}

std::optional<DataChunk> DataChunk::parse(const Chunk &chunk)
{
    constexpr std::size_t fields_size = HeaderSize - Chunk::HeaderSize;
    if (chunk.value.size() < fields_size) {
        return std::nullopt;
    }
    return DataChunk{
        .tsn = load32(chunk.value, 0),
        .stream = load16(chunk.value, 4),
        .ssn = load16(chunk.value, 6),
        .ppid = load32(chunk.value, 8),
        .flags = chunk.flags,
        .payload = chunk.value.subspan(fields_size),
    };
}

std::optional<InitChunk> InitChunk::parse(const Chunk &chunk)
{
    constexpr std::size_t fields_size = 16;
    if (chunk.value.size() < fields_size) {
        return std::nullopt;
    }
    auto parameters = Tlv::parseAll(chunk.value.subspan(fields_size));
    if (!parameters) {
        return std::nullopt;
    }
    return InitChunk{
        .initiate_tag = load32(chunk.value, 0),
        .a_rwnd = load32(chunk.value, 4),
        .outbound_streams = load16(chunk.value, 8),
        .inbound_streams = load16(chunk.value, 10),
        .initial_tsn = load32(chunk.value, 12),
        .parameters = std::move(*parameters),
    };
}

std::optional<SackChunk> SackChunk::parse(const Chunk &chunk)
{
    constexpr std::size_t fields_size = 12;
    if (chunk.value.size() < fields_size) {
        return std::nullopt;
    }
    const std::size_t gap_count = load16(chunk.value, 8);
    const std::size_t duplicate_count = load16(chunk.value, 10);
    if (chunk.value.size() != fields_size + 4 * (gap_count + duplicate_count)) {
        return std::nullopt;
    }
    SackChunk sack{
        .cumulative_tsn_ack = load32(chunk.value, 0),
        .a_rwnd = load32(chunk.value, 4),
        .gap_blocks = {},
        .duplicate_tsns = {},
    };
    sack.gap_blocks.reserve(gap_count);
    for (std::size_t i = 0; i < gap_count; ++i) {
        const auto offset = fields_size + 4 * i;
        sack.gap_blocks.push_back({.start = load16(chunk.value, offset), .end = load16(chunk.value, offset + 2)});
    }
    sack.duplicate_tsns.reserve(duplicate_count);
    for (std::size_t i = 0; i < duplicate_count; ++i) {
        sack.duplicate_tsns.push_back(load32(chunk.value, fields_size + 4 * (gap_count + i)));
    }
    return sack;
}

std::optional<ForwardTsnChunk> ForwardTsnChunk::parse(const Chunk &chunk)
{
    if (chunk.value.size() < 4 || chunk.value.size() % 4 != 0) {
        return std::nullopt;
    }
    ForwardTsnChunk forward_tsn{.new_cumulative_tsn = load32(chunk.value, 0), .skipped = {}};
    forward_tsn.skipped.reserve((chunk.value.size() - 4) / 4);
    for (std::size_t offset = 4; offset < chunk.value.size(); offset += 4) {
        forward_tsn.skipped.push_back({.stream = load16(chunk.value, offset), .ssn = load16(chunk.value, offset + 2)});
    }
    return forward_tsn;
}

std::optional<OutgoingResetRequest> OutgoingResetRequest::parse(const Tlv &parameter)
{
    if (parameter.value.size() < 12) {
        return std::nullopt;
    }
    auto streams = parseStreams(parameter.value.subspan(12));
    if (!streams) {
        return std::nullopt;
    }
    return OutgoingResetRequest{
        .request_sequence = load32(parameter.value, 0),
        .response_sequence = load32(parameter.value, 4),
        .last_assigned_tsn = load32(parameter.value, 8),
        .streams = std::move(*streams),
    };
}

std::optional<IncomingResetRequest> IncomingResetRequest::parse(const Tlv &parameter)
{
    if (parameter.value.size() < 4) {
        return std::nullopt;
    }
    auto streams = parseStreams(parameter.value.subspan(4));
    if (!streams) {
        return std::nullopt;
    }
    return IncomingResetRequest{.request_sequence = load32(parameter.value, 0), .streams = std::move(*streams)};
}

std::optional<ReconfigResponse> ReconfigResponse::parse(const Tlv &parameter)
{
    if (parameter.value.size() != 8 && parameter.value.size() != 16) {
        return std::nullopt;
    }
    const auto result = load32(parameter.value, 4);
    if (result > static_cast<std::uint32_t>(ReconfigResult::InProgress)) {
        return std::nullopt;
    }
    return ReconfigResponse{
        .response_sequence = load32(parameter.value, 0),
        .result = static_cast<ReconfigResult>(result),
    };
}

void ByteWriter::write8(std::uint8_t value)
{
    out_.push_back(static_cast<std::byte>(value));
}

void ByteWriter::write16(std::uint16_t value)
{
    write8(static_cast<std::uint8_t>(value >> 8U));
    write8(static_cast<std::uint8_t>(value));
}

void ByteWriter::write32(std::uint32_t value)
{
    write16(static_cast<std::uint16_t>(value >> 16U));
    write16(static_cast<std::uint16_t>(value));
}

void ByteWriter::write64(std::uint64_t value)
{
    write32(static_cast<std::uint32_t>(value >> 32U));
    write32(static_cast<std::uint32_t>(value));
}

void ByteWriter::write(std::span<const std::byte> bytes)
{
    out_.insert(out_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::writeTlv(std::uint16_t type, std::span<const std::byte> value)
{
    pad();
    write16(type);
    write16(static_cast<std::uint16_t>(Tlv::HeaderSize + value.size()));
    write(value);
}

void ByteWriter::pad()
{
    out_.resize(padded(out_.size()));
}

PacketBuilder::PacketBuilder(std::vector<std::byte> &buffer, const CommonHeader &header, std::size_t max_size)
    : buffer_(buffer), max_size_(max_size & ~std::size_t{3})
{
    buffer_.clear();
    ByteWriter writer(buffer_);
    writer.write16(header.source_port);
    writer.write16(header.destination_port);
    writer.write32(header.verification_tag);
    writer.write32(0);
}

std::size_t PacketBuilder::remaining() const
{
    return buffer_.size() >= max_size_ ? 0 : max_size_ - buffer_.size();
}

void PacketBuilder::addChunk(ChunkType type, std::uint8_t flags, std::span<const std::byte> value)
{
    ByteWriter writer(buffer_);
    writer.write8(static_cast<std::uint8_t>(type));
    writer.write8(flags);
    writer.write16(static_cast<std::uint16_t>(Chunk::HeaderSize + value.size()));
    writer.write(value);
    writer.pad();
}

void PacketBuilder::addData(const DataChunk &header, const ByteBuf &payload)
{
    ByteWriter writer(buffer_);
    writer.write8(static_cast<std::uint8_t>(ChunkType::Data));
    writer.write8(header.flags);
    writer.write16(static_cast<std::uint16_t>(DataChunk::HeaderSize + payload.length()));
    writer.write32(header.tsn);
    writer.write16(header.stream);
    writer.write16(header.ssn);
    writer.write32(header.ppid);
    const auto offset = buffer_.size();
    buffer_.resize(offset + payload.length());
    payload.copyTo(std::span{buffer_}.subspan(offset));
    writer.pad();
}

void PacketBuilder::addInit(ChunkType type, const InitChunk &init, std::span<const std::byte> parameters)
{
    ByteWriter writer(buffer_);
    writer.write8(static_cast<std::uint8_t>(type));
    writer.write8(0);
    writer.write16(static_cast<std::uint16_t>(Chunk::HeaderSize + 16 + parameters.size()));
    writer.write32(init.initiate_tag);
    writer.write32(init.a_rwnd);
    writer.write16(init.outbound_streams);
    writer.write16(init.inbound_streams);
    writer.write32(init.initial_tsn);
    writer.write(parameters);
    writer.pad();
}

void PacketBuilder::addSack(const SackChunk &sack)
{
    ByteWriter writer(buffer_);
    writer.write8(static_cast<std::uint8_t>(ChunkType::Sack));
    writer.write8(0);
    writer.write16(
        static_cast<std::uint16_t>(Chunk::HeaderSize + 12 + 4 * (sack.gap_blocks.size() + sack.duplicate_tsns.size())));
    writer.write32(sack.cumulative_tsn_ack);
    writer.write32(sack.a_rwnd);
    writer.write16(static_cast<std::uint16_t>(sack.gap_blocks.size()));
    writer.write16(static_cast<std::uint16_t>(sack.duplicate_tsns.size()));
    for (const auto &block : sack.gap_blocks) {
        writer.write16(block.start);
        writer.write16(block.end);
    }
    for (const auto tsn : sack.duplicate_tsns) {
        writer.write32(tsn);
    }
}

void PacketBuilder::addForwardTsn(const ForwardTsnChunk &forward_tsn)
{
    ByteWriter writer(buffer_);
    writer.write8(static_cast<std::uint8_t>(ChunkType::ForwardTsn));
    writer.write8(0);
    writer.write16(static_cast<std::uint16_t>(Chunk::HeaderSize + 4 + 4 * forward_tsn.skipped.size()));
    writer.write32(forward_tsn.new_cumulative_tsn);
    for (const auto &skipped : forward_tsn.skipped) {
        writer.write16(skipped.stream);
        writer.write16(skipped.ssn);
    }
}

std::span<const std::byte> PacketBuilder::build()
{
    const auto checksum = crc32c(buffer_);
    buffer_[8] = static_cast<std::byte>(checksum);
    buffer_[9] = static_cast<std::byte>(checksum >> 8U);
    buffer_[10] = static_cast<std::byte>(checksum >> 16U);
    buffer_[11] = static_cast<std::byte>(checksum >> 24U);
    return buffer_;
}

}  // namespace endgate::rtc::sctp
