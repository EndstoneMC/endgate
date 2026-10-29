#include "endgate/network/rtc/sctp/dcep.h"

#include <algorithm>
#include <limits>
#include <span>

namespace endgate::rtc {
namespace {

constexpr std::uint8_t UNORDERED_BIT = 0x80;
constexpr std::uint8_t RELIABLE = 0x00;
constexpr std::uint8_t PARTIAL_RELIABLE_REXMIT = 0x01;
constexpr std::uint8_t PARTIAL_RELIABLE_TIMED = 0x02;

}  // namespace

std::optional<DataChannelOpen> DataChannelOpen::parse(ByteBufView message)
{
    constexpr std::size_t fields_size = 12;
    if (!message.isReadable(fields_size) || message.readUnsignedByte() != MESSAGE_TYPE) {
        return std::nullopt;
    }
    const auto channel_type = message.readUnsignedByte();
    const auto priority = message.readUnsignedShort();
    const auto reliability_parameter = message.readUnsignedInt();
    const std::size_t label_length = message.readUnsignedShort();
    const std::size_t protocol_length = message.readUnsignedShort();
    if (message.readableBytes() != label_length + protocol_length) {
        return std::nullopt;
    }
    const auto label = message.readSlice(label_length).nioBuffer();
    const auto protocol = message.readSlice(protocol_length).nioBuffer();
    return DataChannelOpen{
        .channel_type = channel_type,
        .priority = priority,
        .reliability_parameter = reliability_parameter,
        .label = std::string(reinterpret_cast<const char *>(label.data()), label.size()),
        .protocol = std::string(reinterpret_cast<const char *>(protocol.data()), protocol.size()),
    };
}

DataChannelOpen DataChannelOpen::of(std::string label, std::string protocol, const Reliability &reliability,
                                    std::uint16_t priority)
{
    auto channel_type = RELIABLE;
    std::uint32_t parameter = 0;
    if (reliability.max_packet_life_time) {
        channel_type = PARTIAL_RELIABLE_TIMED;
        parameter = static_cast<std::uint32_t>(std::clamp<std::chrono::milliseconds::rep>(
            reliability.max_packet_life_time->count(), 0, std::numeric_limits<std::uint32_t>::max()));
    }
    else if (reliability.max_retransmits) {
        channel_type = PARTIAL_RELIABLE_REXMIT;
        parameter = *reliability.max_retransmits;
    }
    if (reliability.unordered) {
        channel_type |= UNORDERED_BIT;
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
    Reliability result{.unordered = (channel_type & UNORDERED_BIT) != 0,
                       .max_retransmits = std::nullopt,
                       .max_packet_life_time = std::nullopt};
    switch (channel_type & ~unsigned{UNORDERED_BIT}) {
    case RELIABLE:
        return result;
    case PARTIAL_RELIABLE_REXMIT:
        result.max_retransmits = reliability_parameter;
        return result;
    case PARTIAL_RELIABLE_TIMED:
        result.max_packet_life_time = std::chrono::milliseconds{reliability_parameter};
        return result;
    default:
        return std::nullopt;
    }
}

ByteBuf DataChannelOpen::serialize() const
{
    auto out = Unpooled::buffer(12 + label.size() + protocol.size());
    out.writeByte(MESSAGE_TYPE)
        .writeByte(channel_type)
        .writeShort(priority)
        .writeInt(reliability_parameter)
        .writeShort(static_cast<std::uint16_t>(label.size()))
        .writeShort(static_cast<std::uint16_t>(protocol.size()))
        .writeBytes(std::as_bytes(std::span{label}))
        .writeBytes(std::as_bytes(std::span{protocol}));
    return out;
}

}  // namespace endgate::rtc
