#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "endgate/buffer/bytebuf.h"

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
    static constexpr std::size_t SIZE = 12;

    std::uint16_t source_port;
    std::uint16_t destination_port;
    std::uint32_t verification_tag;

    void write(ByteBuf &out) const;
};

/**
 * A chunk as it sits in a received packet. The value excludes the four byte chunk header and the padding.
 *
 * @see RFC 9260 section 3.2
 */
struct Chunk {
    static constexpr std::size_t HEADER_SIZE = 4;

    std::uint8_t type;
    std::uint8_t flags;
    ByteBufView value;

    void write(ByteBuf &out) const;
};

/**
 * @see dcsctp::SctpPacket
 */
struct Packet {
    CommonHeader header;
    std::vector<Chunk> chunks;

    static std::optional<Packet> parse(ByteBufView data);

    /**
     * Fills in the CRC32c of a packet whose header was written with CommonHeader::write.
     *
     * @see RFC 9260 section 6.8
     */
    static void seal(ByteBuf &packet);
};

/**
 * A parameter or an error cause; both share this layout.
 *
 * @see RFC 9260 section 3.2.1
 */
struct Tlv {
    static constexpr std::size_t HEADER_SIZE = 4;

    std::uint16_t type;
    ByteBufView value;
    ByteBufView raw;

    static std::optional<std::vector<Tlv>> parseAll(ByteBufView data);

    void write(ByteBuf &out) const;
};

/**
 * @see RFC 9260 section 3.3.1
 */
struct DataChunk {
    static constexpr std::size_t HEADER_SIZE = 16;
    static constexpr std::uint8_t END_FLAG = 0x01;
    static constexpr std::uint8_t BEGINNING_FLAG = 0x02;
    static constexpr std::uint8_t UNORDERED_FLAG = 0x04;
    static constexpr std::uint8_t IMMEDIATE_FLAG = 0x08;

    std::uint32_t tsn;
    std::uint16_t stream;
    std::uint16_t ssn;
    std::uint32_t ppid;
    std::uint8_t flags;
    ByteBufView payload;

    static std::optional<DataChunk> parse(const Chunk &chunk);

    void write(ByteBuf &out) const;
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

    void write(ByteBuf &out, ChunkType type, ByteBufView encoded_parameters) const;
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

    void write(ByteBuf &out) const;
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

    void write(ByteBuf &out) const;
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

}  // namespace endgate::rtc::sctp
