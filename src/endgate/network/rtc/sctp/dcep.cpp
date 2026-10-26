#include "endgate/network/rtc/sctp/dcep.h"

#include <algorithm>
#include <limits>

#include "endgate/network/rtc/sctp/packet.h"

namespace endgate::rtc {
namespace {

constexpr std::uint8_t UnorderedBit = 0x80;
constexpr std::uint8_t Reliable = 0x00;
constexpr std::uint8_t PartialReliableRexmit = 0x01;
constexpr std::uint8_t PartialReliableTimed = 0x02;

}  // namespace

std::optional<DataChannelOpen> DataChannelOpen::parse(std::span<const std::byte> message)
{
    constexpr std::size_t fields_size = 12;
    if (message.size() < fields_size || std::to_integer<std::uint8_t>(message[0]) != MessageType) {
        return std::nullopt;
    }
    const auto load16 = [&](std::size_t offset) {
        return static_cast<std::uint16_t>(std::to_integer<unsigned>(message[offset]) << 8U |
                                          std::to_integer<unsigned>(message[offset + 1]));
    };
    const std::size_t label_length = load16(8);
    const std::size_t protocol_length = load16(10);
    if (message.size() != fields_size + label_length + protocol_length) {
        return std::nullopt;
    }
    const auto text = [&](std::size_t offset, std::size_t length) {
        return std::string(reinterpret_cast<const char *>(message.data() + offset), length);
    };
    return DataChannelOpen{
        .channel_type = std::to_integer<std::uint8_t>(message[1]),
        .priority = load16(2),
        .reliability_parameter = static_cast<std::uint32_t>(load16(4)) << 16U | load16(6),
        .label = text(fields_size, label_length),
        .protocol = text(fields_size + label_length, protocol_length),
    };
}

DataChannelOpen DataChannelOpen::of(std::string label, std::string protocol, const Reliability &reliability,
                                    std::uint16_t priority)
{
    auto channel_type = Reliable;
    std::uint32_t parameter = 0;
    if (reliability.max_packet_life_time) {
        channel_type = PartialReliableTimed;
        parameter = static_cast<std::uint32_t>(std::clamp<std::chrono::milliseconds::rep>(
            reliability.max_packet_life_time->count(), 0, std::numeric_limits<std::uint32_t>::max()));
    }
    else if (reliability.max_retransmits) {
        channel_type = PartialReliableRexmit;
        parameter = *reliability.max_retransmits;
    }
    if (reliability.unordered) {
        channel_type |= UnorderedBit;
    }
    return {
        .channel_type = channel_type,
        .priority = priority,
        .reliability_parameter = parameter,
        .label = std::move(label),
        .protocol = std::move(protocol),
    };
}

std::optional<Reliability> DataChannelOpen::reliability() const
{
    Reliability result{.unordered = (channel_type & UnorderedBit) != 0,
                       .max_retransmits = std::nullopt,
                       .max_packet_life_time = std::nullopt};
    switch (channel_type & ~unsigned{UnorderedBit}) {
    case Reliable:
        return result;
    case PartialReliableRexmit:
        result.max_retransmits = reliability_parameter;
        return result;
    case PartialReliableTimed:
        result.max_packet_life_time = std::chrono::milliseconds{reliability_parameter};
        return result;
    default:
        return std::nullopt;
    }
}

std::vector<std::byte> DataChannelOpen::serialize() const
{
    std::vector<std::byte> out;
    out.reserve(12 + label.size() + protocol.size());
    sctp::ByteWriter writer(out);
    writer.write8(MessageType);
    writer.write8(channel_type);
    writer.write16(priority);
    writer.write32(reliability_parameter);
    writer.write16(static_cast<std::uint16_t>(label.size()));
    writer.write16(static_cast<std::uint16_t>(protocol.size()));
    writer.write(std::as_bytes(std::span{label}));
    writer.write(std::as_bytes(std::span{protocol}));
    return out;
}

}  // namespace endgate::rtc
