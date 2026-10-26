#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "endgate/network/buffer.h"

namespace endgate::rtc::sctp {

/**
 * @see RFC 9260 section 3.2
 */
enum class ChunkType : std::uint8_t {
    Data = 0,
    Init = 1,
    InitAck = 2,
    Sack = 3,
    Heartbeat = 4,
    HeartbeatAck = 5,
    Abort = 6,
    Shutdown = 7,
    ShutdownAck = 8,
    Error = 9,
    CookieEcho = 10,
    CookieAck = 11,
    ShutdownComplete = 14,
    IData = 64,
    ReConfig = 130,
    ForwardTsn = 192,
    IForwardTsn = 194,
};

/**
 * @see RFC 9260 section 3.2.1
 * @see RFC 6525 section 4
 */
enum class ParameterType : std::uint16_t {
    HeartbeatInfo = 1,
    Ipv4Address = 5,
    Ipv6Address = 6,
    StateCookie = 7,
    UnrecognizedParameter = 8,
    CookiePreservative = 9,
    HostNameAddress = 11,
    SupportedAddressTypes = 12,
    OutgoingSsnResetRequest = 13,
    IncomingSsnResetRequest = 14,
    SsnTsnResetRequest = 15,
    ReconfigurationResponse = 16,
    AddOutgoingStreamsRequest = 17,
    AddIncomingStreamsRequest = 18,
    SupportedExtensions = 0x8008,
    ForwardTsnSupported = 0xC000,
};

/**
 * @see RFC 9260 section 3.3.10
 */
enum class CauseCode : std::uint16_t {
    InvalidStreamIdentifier = 1,
    MissingMandatoryParameter = 2,
    StaleCookie = 3,
    OutOfResource = 4,
    UnrecognizedChunkType = 6,
    InvalidMandatoryParameter = 7,
    UnrecognizedParameters = 8,
    NoUserData = 9,
    CookieReceivedWhileShuttingDown = 10,
    UserInitiatedAbort = 12,
    ProtocolViolation = 13,
};

/**
 * @see RFC 6525 section 4.4
 */
enum class ReconfigResult : std::uint32_t {
    SuccessNothingToDo = 0,
    SuccessPerformed = 1,
    Denied = 2,
    ErrorWrongSsn = 3,
    ErrorRequestAlreadyInProgress = 4,
    ErrorBadSequenceNumber = 5,
    InProgress = 6,
};

/**
 * @see RFC 9260 section 3.1
 */
struct CommonHeader {
    static constexpr std::size_t size = 12;

    std::uint16_t source_port;
    std::uint16_t destination_port;
    std::uint32_t verification_tag;
};

/**
 * A chunk as it sits in a received packet. The value excludes the four byte chunk header and the padding.
 *
 * @see RFC 9260 section 3.2
 */
struct Chunk {
    static constexpr std::size_t header_size = 4;

    std::uint8_t type;
    std::uint8_t flags;
    std::span<const std::byte> value;
};

/**
 * @see dcsctp::SctpPacket
 */
struct Packet {
    CommonHeader header;
    std::vector<Chunk> chunks;

    static std::optional<Packet> parse(std::span<const std::byte> data);
};

/**
 * A parameter or an error cause; both share this layout.
 *
 * @see RFC 9260 section 3.2.1
 */
struct Tlv {
    static constexpr std::size_t header_size = 4;

    std::uint16_t type;
    std::span<const std::byte> value;
    std::span<const std::byte> raw;

    static std::optional<std::vector<Tlv>> parseAll(std::span<const std::byte> data);
};

/**
 * @see RFC 9260 section 3.3.1
 */
struct DataChunk {
    static constexpr std::size_t header_size = 16;
    static constexpr std::uint8_t end_flag = 0x01;
    static constexpr std::uint8_t beginning_flag = 0x02;
    static constexpr std::uint8_t unordered_flag = 0x04;
    static constexpr std::uint8_t immediate_flag = 0x08;

    std::uint32_t tsn;
    std::uint16_t stream;
    std::uint16_t ssn;
    std::uint32_t ppid;
    std::uint8_t flags;
    std::span<const std::byte> payload;

    static std::optional<DataChunk> parse(const Chunk &chunk);
};

/**
 * INIT and INIT ACK share one layout.
 *
 * @see RFC 9260 section 3.3.2
 * @see RFC 9260 section 3.3.3
 */
struct InitChunk {
    std::uint32_t initiate_tag;
    std::uint32_t a_rwnd;
    std::uint16_t outbound_streams;
    std::uint16_t inbound_streams;
    std::uint32_t initial_tsn;
    std::vector<Tlv> parameters;

    static std::optional<InitChunk> parse(const Chunk &chunk);
};

/**
 * @see RFC 9260 section 3.3.4
 */
struct SackChunk {
    struct GapBlock {
        std::uint16_t start;
        std::uint16_t end;

        bool operator==(const GapBlock &) const = default;
    };

    std::uint32_t cumulative_tsn_ack;
    std::uint32_t a_rwnd;
    std::vector<GapBlock> gap_blocks;
    std::vector<std::uint32_t> duplicate_tsns;

    static std::optional<SackChunk> parse(const Chunk &chunk);
};

/**
 * @see RFC 3758 section 3.2
 */
struct ForwardTsnChunk {
    struct SkippedStream {
        std::uint16_t stream;
        std::uint16_t ssn;

        bool operator==(const SkippedStream &) const = default;
    };

    std::uint32_t new_cumulative_tsn;
    std::vector<SkippedStream> skipped;

    static std::optional<ForwardTsnChunk> parse(const Chunk &chunk);
};

/**
 * @see RFC 6525 section 4.1
 */
struct OutgoingResetRequest {
    std::uint32_t request_sequence;
    std::uint32_t response_sequence;
    std::uint32_t last_assigned_tsn;
    std::vector<std::uint16_t> streams;

    static std::optional<OutgoingResetRequest> parse(const Tlv &parameter);
};

/**
 * @see RFC 6525 section 4.2
 */
struct IncomingResetRequest {
    std::uint32_t request_sequence;
    std::vector<std::uint16_t> streams;

    static std::optional<IncomingResetRequest> parse(const Tlv &parameter);
};

/**
 * @see RFC 6525 section 4.4
 */
struct ReconfigResponse {
    std::uint32_t response_sequence;
    ReconfigResult result;

    static std::optional<ReconfigResponse> parse(const Tlv &parameter);
};

/**
 * Appends big-endian fields and padded TLVs to a byte vector.
 *
 * @see dcsctp::BoundedByteWriter
 */
class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::byte> &out) : out_(out) {}

    void write8(std::uint8_t value);
    void write16(std::uint16_t value);
    void write32(std::uint32_t value);
    void write64(std::uint64_t value);
    void write(std::span<const std::byte> bytes);
    void writeTlv(std::uint16_t type, std::span<const std::byte> value);
    void pad();

private:
    std::vector<std::byte> &out_;
};

/**
 * Lays chunks out behind a common header, never past the size it was given, and seals the packet with its CRC32c.
 *
 * @see dcsctp::SctpPacket::Builder
 */
class PacketBuilder {
public:
    PacketBuilder(std::vector<std::byte> &buffer, const CommonHeader &header, std::size_t max_size);

    [[nodiscard]] bool empty() const { return buffer_.size() == CommonHeader::size; }

    [[nodiscard]] std::size_t remaining() const;

    void addChunk(ChunkType type, std::uint8_t flags, std::span<const std::byte> value);
    void addData(const DataChunk &header, const ByteBuf &payload);
    void addInit(ChunkType type, const InitChunk &init, std::span<const std::byte> parameters);
    void addSack(const SackChunk &sack);
    void addForwardTsn(const ForwardTsnChunk &forward_tsn);

    std::span<const std::byte> build();

private:
    std::vector<std::byte> &buffer_;
    std::size_t max_size_;
};

}  // namespace endgate::rtc::sctp
