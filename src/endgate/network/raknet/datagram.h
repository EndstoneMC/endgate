#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/raknet/protocol.h"

namespace endgate::raknet {

inline constexpr std::uint32_t UINT24_MASK = 0xFFFFFF;

/**
 * @see DatagramHeaderFormat
 */
struct DatagramHeader {
    bool is_ack{false};
    bool is_nak{false};
    bool is_packet_pair{false};
    bool is_continuous_send{false};
    bool needs_b_and_as{false};
    bool has_b_and_as{false};
    float as{0};
    std::uint32_t datagram_number{0};

    /**
     * @see DatagramHeaderFormat::GetDataHeaderByteLength
     */
    static constexpr std::size_t DATA_HEADER_BYTE_LENGTH = 2 + 3 + sizeof(float);

    [[nodiscard]] static std::expected<DatagramHeader, std::string_view> decode(ByteBufView &reader);
    void encode(ByteBuf &writer) const;
};

/**
 * @see InternalPacketFixedSizeTransmissionHeader
 */
struct FrameHeader {
    PacketReliability reliability{PacketReliability::Unreliable};
    std::uint32_t reliable_message_number{UINT24_MASK};
    std::uint32_t sequencing_index{0};
    std::uint32_t ordering_index{0};
    std::uint8_t ordering_channel{0};
    std::uint32_t split_packet_count{0};
    std::uint16_t split_packet_id{0};
    std::uint32_t split_packet_index{0};

    /**
     * @see ReliabilityLayer::GetMessageHeaderLengthBits
     */
    [[nodiscard]] std::size_t length() const;

    /**
     * @see ReliabilityLayer::WriteToBitStreamFromInternalPacket
     */
    void encode(ByteBuf &writer, std::size_t data_byte_length) const;
};

/**
 * @see ReliabilityLayer::GetMaxMessageHeaderLengthBits
 */
inline constexpr std::size_t MAXIMUM_FRAME_HEADER_LENGTH = 1 + 2 + 3 + 3 + 3 + 1 + 4 + 2 + 4;

/**
 * @see InternalPacket
 */
struct Frame {
    FrameHeader header;
    ByteBufView data;

    static constexpr std::size_t MINIMUM_LENGTH = 4;

    /**
     * @see ReliabilityLayer::CreateInternalPacketFromBitStream
     */
    [[nodiscard]] static std::expected<Frame, std::string_view> decode(ByteBufView &reader);
};

/**
 * @see DataStructures::RangeNode
 */
struct Range {
    std::uint32_t min;
    std::uint32_t max;

    friend bool operator==(const Range &, const Range &) = default;
};

enum class RangeListError {
    Truncated,
    TooManyRanges,
};

/**
 * @see DataStructures::RangeList
 */
class RangeList {
public:
    void insert(std::uint32_t index);
    void clear() { ranges_.clear(); }

    [[nodiscard]] bool empty() const { return ranges_.empty(); }
    [[nodiscard]] const std::vector<Range> &ranges() const { return ranges_; }

    /**
     * @see DataStructures::RangeList::Serialize
     */
    void serialize(ByteBuf &writer, std::size_t max_bits);

    /**
     * @see DataStructures::RangeList::Deserialize
     */
    [[nodiscard]] static std::expected<std::vector<Range>, RangeListError> deserialize(ByteBufView &reader);

    static constexpr std::size_t MAX_ACKNOWLEDGEMENT_RANGES_PER_DATAGRAM = 0x400;
    static constexpr std::uint32_t MAX_ACKNOWLEDGEMENTS_PER_DATAGRAM = 0x4000;

private:
    std::vector<Range> ranges_;
};

}  // namespace endgate::raknet
