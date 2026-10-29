#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "bytes.h"
#include "endgate/network/rtc/sctp/packet.h"

namespace endgate::rtc::sctp {
namespace {

Chunk chunkOf(std::span<const std::byte> serialized)
{
    const auto length = static_cast<std::size_t>(std::to_integer<std::uint8_t>(serialized[2])) << 8U |
                        std::to_integer<std::uint8_t>(serialized[3]);
    return {
        .type = std::to_integer<std::uint8_t>(serialized[0]),
        .flags = std::to_integer<std::uint8_t>(serialized[1]),
        .value = serialized.subspan(4, length - 4),
    };
}

std::vector<std::byte> serialized(auto &&add)
{
    std::vector<std::byte> buffer;
    PacketBuilder builder(buffer, {}, 65536);
    add(builder);
    const auto packet = builder.build();
    return {packet.begin() + CommonHeader::SIZE, packet.end()};
}

std::vector<std::byte> copy(std::span<const std::byte> span)
{
    return {span.begin(), span.end()};
}

const std::vector<std::byte> CAPTURED_DATA = bytes({0x00, 0x03, 0x00, 0x14, 0x55, 0x08, 0x36, 0x3c, 0x00, 0x02,
                                                    0x00, 0x01, 0x00, 0x00, 0x00, 0x35, 0x00, 0x01, 0x02, 0x03});

const std::vector<std::byte> CAPTURED_INIT = bytes({
    0x01, 0x00, 0x00, 0x5a, 0xde, 0x7a, 0x16, 0x90, 0x00, 0x02, 0x00, 0x00, 0x03, 0xe8, 0x03, 0xe8, 0x25, 0x0d, 0x37,
    0xe8, 0x80, 0x00, 0x00, 0x04, 0xc0, 0x00, 0x00, 0x04, 0x80, 0x08, 0x00, 0x09, 0xc0, 0x0f, 0xc1, 0x80, 0x82, 0x00,
    0x00, 0x00, 0x80, 0x02, 0x00, 0x24, 0xab, 0x31, 0x44, 0x62, 0x12, 0x1a, 0x15, 0x13, 0xfd, 0x5a, 0x5f, 0x69, 0xef,
    0xaa, 0x06, 0xe9, 0xab, 0xd7, 0x48, 0xcc, 0x3b, 0xd1, 0x4b, 0x60, 0xed, 0x7f, 0xa6, 0x44, 0xce, 0x4d, 0xd2, 0xad,
    0x80, 0x04, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x80, 0x03, 0x00, 0x06, 0x80, 0xc1, 0x00, 0x00,
});

const std::vector<std::byte> CAPTURED_INIT_ACK = bytes({
    0x02, 0x00, 0x01, 0x24, 0x57, 0x9c, 0x2f, 0x98, 0x00, 0x02, 0x00, 0x00, 0x03, 0xe8, 0x08, 0x00, 0x63, 0x96, 0x8e,
    0xc7, 0xc0, 0x00, 0x00, 0x04, 0x80, 0x08, 0x00, 0x06, 0xc0, 0x82, 0x00, 0x00, 0x00, 0x07, 0x01, 0x04, 0x4b, 0x41,
    0x4d, 0x45, 0x2d, 0x42, 0x53, 0x44, 0x20, 0x31, 0x2e, 0x31, 0x00, 0x00, 0x00, 0x00, 0x96, 0xb8, 0x38, 0x60, 0x00,
    0x00, 0x00, 0x00, 0x52, 0x5a, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0xea, 0x00, 0x00, 0xb5, 0xaa, 0x19, 0xea,
    0x31, 0xef, 0xa4, 0x2b, 0x90, 0x16, 0x7a, 0xde, 0x57, 0x9c, 0x2f, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x13, 0x88, 0x13, 0x88, 0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x5a, 0xde, 0x7a, 0x16, 0x90, 0x00, 0x02, 0x00, 0x00, 0x03, 0xe8, 0x03, 0xe8, 0x25, 0x0d, 0x37,
    0xe8, 0x80, 0x00, 0x00, 0x04, 0xc0, 0x00, 0x00, 0x04, 0x80, 0x08, 0x00, 0x09, 0xc0, 0x0f, 0xc1, 0x80, 0x82, 0x00,
    0x00, 0x00, 0x80, 0x02, 0x00, 0x24, 0xab, 0x31, 0x44, 0x62, 0x12, 0x1a, 0x15, 0x13, 0xfd, 0x5a, 0x5f, 0x69, 0xef,
    0xaa, 0x06, 0xe9, 0xab, 0xd7, 0x48, 0xcc, 0x3b, 0xd1, 0x4b, 0x60, 0xed, 0x7f, 0xa6, 0x44, 0xce, 0x4d, 0xd2, 0xad,
    0x80, 0x04, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x80, 0x03, 0x00, 0x06, 0x80, 0xc1, 0x00, 0x00, 0x02, 0x00, 0x01,
    0x24, 0x57, 0x9c, 0x2f, 0x98, 0x00, 0x02, 0x00, 0x00, 0x03, 0xe8, 0x08, 0x00, 0x63, 0x96, 0x8e, 0xc7, 0xc0, 0x00,
    0x00, 0x04, 0x80, 0x08, 0x00, 0x06, 0xc0, 0x82, 0x00, 0x00, 0x51, 0x95, 0x01, 0x88, 0x0d, 0x80, 0x7b, 0x19, 0xe7,
    0xf9, 0xc6, 0x18, 0x5c, 0x4a, 0xbf, 0x39, 0x32, 0xe5, 0x63, 0x8e,
});

const std::vector<std::byte> CAPTURED_SACK =
    bytes({0x03, 0x00, 0x00, 0x1c, 0x36, 0x9d, 0xd0, 0x0b, 0x00, 0x01, 0xed, 0x73, 0x00, 0x02,
           0x00, 0x01, 0x00, 0x02, 0x00, 0x06, 0x00, 0x08, 0x00, 0x08, 0x36, 0x9d, 0xd0, 0x11});

const std::vector<std::byte> CAPTURED_RECONFIG =
    bytes({0x82, 0x00, 0x00, 0x16, 0x00, 0x0d, 0x00, 0x12, 0x87, 0x55, 0xd8, 0x23,
           0x71, 0x97, 0x6a, 0x9e, 0x87, 0x55, 0xd8, 0x32, 0x00, 0x06, 0x00, 0x00});

const std::vector<std::byte> CAPTURED_HEARTBEAT = bytes({
    0x04, 0x00, 0x00, 0x2c, 0x00, 0x01, 0x00, 0x28, 0xad, 0x24, 0x36, 0x60, 0x37, 0x26, 0x07,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7b, 0x10, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
});

TEST(DataChunk, ParsesCapture)
{
    const auto data = DataChunk::parse(chunkOf(CAPTURED_DATA));
    ASSERT_TRUE(data);
    EXPECT_EQ(data->tsn, 1426601532U);
    EXPECT_EQ(data->stream, 2);
    EXPECT_EQ(data->ssn, 1);
    EXPECT_EQ(data->ppid, 53U);
    EXPECT_EQ(data->flags, DataChunk::BEGINNING_FLAG | DataChunk::END_FLAG);
    EXPECT_EQ(copy(data->payload), bytes({0, 1, 2, 3}));
}

TEST(DataChunk, SerializesCapture)
{
    EXPECT_EQ(serialized([](PacketBuilder &builder) {
                  builder.addData({.tsn = 1426601532, .stream = 2, .ssn = 1, .ppid = 53, .flags = 3, .payload = {}},
                                  LegacyByteBuf::copyBuffer(bytes({0, 1, 2, 3})));
              }),
              CAPTURED_DATA);
}

TEST(DataChunk, PadsOddPayload)
{
    const auto chunk = serialized([](PacketBuilder &builder) {
        builder.addData({.tsn = 7, .stream = 1, .ssn = 0, .ppid = 51, .flags = 3, .payload = {}},
                        LegacyByteBuf::copyBuffer(bytes({0xaa})));
    });
    EXPECT_EQ(chunk, bytes({0x00, 0x03, 0x00, 0x11, 0, 0, 0, 7, 0, 1, 0, 0, 0, 0, 0, 51, 0xaa, 0, 0, 0}));
}

TEST(DataChunk, SerializesChainedPayload)
{
    auto payload = LegacyByteBuf::copyBuffer(bytes({1, 2}));
    payload.appendToChain(LegacyByteBuf::copyBuffer(bytes({3, 4, 5})));
    const auto chunk = serialized([&](PacketBuilder &builder) {
        builder.addData({.tsn = 1, .stream = 0, .ssn = 0, .ppid = 53, .flags = 3, .payload = {}}, payload);
    });
    const auto data = DataChunk::parse(chunkOf(chunk));
    ASSERT_TRUE(data);
    EXPECT_EQ(copy(data->payload), bytes({1, 2, 3, 4, 5}));
}

TEST(DataChunk, RejectsTruncatedHeader)
{
    const auto truncated = bytes({0x00, 0x03, 0x00, 0x0f, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0});
    EXPECT_FALSE(DataChunk::parse(chunkOf(truncated)));
}

TEST(DataChunk, ParsesEmptyPayload)
{
    const auto empty = bytes({0x00, 0x03, 0x00, 0x10, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto data = DataChunk::parse(chunkOf(empty));
    ASSERT_TRUE(data);
    EXPECT_TRUE(data->payload.empty());
}

TEST(InitChunk, ParsesCapture)
{
    const auto init = InitChunk::parse(chunkOf(CAPTURED_INIT));
    ASSERT_TRUE(init);
    EXPECT_EQ(init->initiate_tag, 0xde7a1690U);
    EXPECT_EQ(init->a_rwnd, 131072U);
    EXPECT_EQ(init->outbound_streams, 1000);
    EXPECT_EQ(init->inbound_streams, 1000);
    EXPECT_EQ(init->initial_tsn, 621623272U);
    ASSERT_EQ(init->parameters.size(), 6U);
    EXPECT_EQ(init->parameters[0].type, 0x8000);
    EXPECT_EQ(init->parameters[1].type, static_cast<std::uint16_t>(ParameterType::ForwardTsnSupported));
    EXPECT_EQ(init->parameters[2].type, static_cast<std::uint16_t>(ParameterType::SupportedExtensions));
    EXPECT_EQ(copy(init->parameters[2].value), bytes({0xc0, 0x0f, 0xc1, 0x80, 0x82}));
    EXPECT_EQ(init->parameters[3].type, 0x8002);
    EXPECT_EQ(init->parameters[3].value.size(), 32U);
    EXPECT_EQ(init->parameters[5].type, 0x8003);
    EXPECT_EQ(copy(init->parameters[5].raw), bytes({0x80, 0x03, 0x00, 0x06, 0x80, 0xc1}));
}

TEST(InitChunk, SerializesCapture)
{
    const auto init = InitChunk::parse(chunkOf(CAPTURED_INIT));
    ASSERT_TRUE(init);
    std::vector<std::byte> parameters;
    ByteWriter writer(parameters);
    for (const auto &parameter : init->parameters) {
        writer.writeTlv(parameter.type, parameter.value);
    }
    EXPECT_EQ(serialized([&](PacketBuilder &builder) { builder.addInit(ChunkType::Init, *init, parameters); }),
              CAPTURED_INIT);
}

TEST(InitChunk, ParsesCapturedInitAckWithKameCookie)
{
    const auto init_ack = InitChunk::parse(chunkOf(CAPTURED_INIT_ACK));
    ASSERT_TRUE(init_ack);
    EXPECT_EQ(init_ack->initiate_tag, 0x579c2f98U);
    EXPECT_EQ(init_ack->outbound_streams, 1000);
    EXPECT_EQ(init_ack->inbound_streams, 2048);
    EXPECT_EQ(init_ack->initial_tsn, 1670811335U);
    ASSERT_EQ(init_ack->parameters.size(), 3U);
    EXPECT_EQ(init_ack->parameters[2].type, static_cast<std::uint16_t>(ParameterType::StateCookie));
    EXPECT_EQ(init_ack->parameters[2].value.size(), 0x104U - 4);
    EXPECT_EQ(copy(init_ack->parameters[2].value.first(8)), bytes({'K', 'A', 'M', 'E', '-', 'B', 'S', 'D'}));
}

TEST(InitChunk, RejectsTruncatedFields)
{
    const auto truncated = bytes({0x01, 0x00, 0x00, 0x13, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0});
    EXPECT_FALSE(InitChunk::parse(chunkOf(truncated)));
}

TEST(InitChunk, RejectsParameterRunningPastChunk)
{
    const auto overrun =
        bytes({0x01, 0x00, 0x00, 0x18, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0xc0, 0x00, 0x00, 0x08});
    EXPECT_FALSE(InitChunk::parse(chunkOf(overrun)));
}

TEST(InitChunk, RejectsParameterShorterThanItsHeader)
{
    const auto short_parameter =
        bytes({0x01, 0x00, 0x00, 0x18, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0xc0, 0x00, 0x00, 0x02});
    EXPECT_FALSE(InitChunk::parse(chunkOf(short_parameter)));
}

TEST(SackChunk, ParsesCapture)
{
    const auto sack = SackChunk::parse(chunkOf(CAPTURED_SACK));
    ASSERT_TRUE(sack);
    EXPECT_EQ(sack->cumulative_tsn_ack, 916312075U);
    EXPECT_EQ(sack->a_rwnd, 126323U);
    EXPECT_EQ(sack->gap_blocks, (std::vector<SackChunk::GapBlock>{{.start = 2, .end = 6}, {.start = 8, .end = 8}}));
    EXPECT_EQ(sack->duplicate_tsns, std::vector<std::uint32_t>{916312081});
}

TEST(SackChunk, SerializesCapture)
{
    EXPECT_EQ(serialized([](PacketBuilder &builder) {
                  builder.addSack({
                      .cumulative_tsn_ack = 916312075,
                      .a_rwnd = 126323,
                      .gap_blocks = {{.start = 2, .end = 6}, {.start = 8, .end = 8}},
                      .duplicate_tsns = {916312081},
                  });
              }),
              CAPTURED_SACK);
}

TEST(SackChunk, RejectsCountsThatDisagreeWithLength)
{
    auto overcounted = CAPTURED_SACK;
    overcounted[13] = std::byte{3};
    EXPECT_FALSE(SackChunk::parse(chunkOf(overcounted)));

    auto undercounted = CAPTURED_SACK;
    undercounted[15] = std::byte{0};
    EXPECT_FALSE(SackChunk::parse(chunkOf(undercounted)));

    const auto truncated = bytes({0x03, 0x00, 0x00, 0x0f, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0});
    EXPECT_FALSE(SackChunk::parse(chunkOf(truncated)));
}

TEST(ForwardTsnChunk, ParsesCapture)
{
    const auto forward_tsn = ForwardTsnChunk::parse(chunkOf(bytes({0xc0, 0x00, 0x00, 0x08, 0x71, 0x97, 0x6b, 0x2a})));
    ASSERT_TRUE(forward_tsn);
    EXPECT_EQ(forward_tsn->new_cumulative_tsn, 1905748778U);
    EXPECT_TRUE(forward_tsn->skipped.empty());
}

TEST(ForwardTsnChunk, RoundTripsSkippedStreams)
{
    const auto chunk = serialized([](PacketBuilder &builder) {
        builder.addForwardTsn(
            {.new_cumulative_tsn = 123, .skipped = {{.stream = 1, .ssn = 23}, {.stream = 42, .ssn = 99}}});
    });
    EXPECT_EQ(chunk, bytes({0xc0, 0x00, 0x00, 0x10, 0, 0, 0, 123, 0, 1, 0, 23, 0, 42, 0, 99}));
    const auto forward_tsn = ForwardTsnChunk::parse(chunkOf(chunk));
    ASSERT_TRUE(forward_tsn);
    EXPECT_EQ(forward_tsn->new_cumulative_tsn, 123U);
    EXPECT_EQ(forward_tsn->skipped,
              (std::vector<ForwardTsnChunk::SkippedStream>{{.stream = 1, .ssn = 23}, {.stream = 42, .ssn = 99}}));
}

TEST(ForwardTsnChunk, RejectsPartialSkippedStream)
{
    EXPECT_FALSE(ForwardTsnChunk::parse(chunkOf(bytes({0xc0, 0x00, 0x00, 0x0a, 0, 0, 0, 1, 0, 1, 0, 0}))));
    EXPECT_FALSE(ForwardTsnChunk::parse(chunkOf(bytes({0xc0, 0x00, 0x00, 0x06, 0, 0, 0, 0}))));
}

TEST(ReconfigChunk, ParsesCapturedOutgoingResetRequest)
{
    const auto chunk = chunkOf(CAPTURED_RECONFIG);
    EXPECT_EQ(chunk.type, static_cast<std::uint8_t>(ChunkType::ReConfig));
    const auto parameters = Tlv::parseAll(chunk.value);
    ASSERT_TRUE(parameters);
    ASSERT_EQ(parameters->size(), 1U);
    EXPECT_EQ((*parameters)[0].type, static_cast<std::uint16_t>(ParameterType::OutgoingSsnResetRequest));
    const auto request = OutgoingResetRequest::parse((*parameters)[0]);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->request_sequence, 2270550051U);
    EXPECT_EQ(request->response_sequence, 1905748638U);
    EXPECT_EQ(request->last_assigned_tsn, 2270550066U);
    EXPECT_EQ(request->streams, std::vector<std::uint16_t>{6});
}

TEST(ReconfigChunk, SerializesCapturedOutgoingResetRequest)
{
    std::vector<std::byte> value;
    std::vector<std::byte> fields;
    ByteWriter field_writer(fields);
    field_writer.write32(2270550051U);
    field_writer.write32(1905748638U);
    field_writer.write32(2270550066U);
    field_writer.write16(6);
    ByteWriter(value).writeTlv(static_cast<std::uint16_t>(ParameterType::OutgoingSsnResetRequest), fields);
    EXPECT_EQ(serialized([&](PacketBuilder &builder) { builder.addChunk(ChunkType::ReConfig, 0, value); }),
              CAPTURED_RECONFIG);
}

TEST(ReconfigChunk, ParsesIncomingResetRequestAndResponse)
{
    const auto incoming = bytes({0x00, 0x0e, 0x00, 0x0c, 0, 0, 0, 5, 0, 1, 0, 3});
    const auto response = bytes({0x00, 0x10, 0x00, 0x0c, 0, 0, 0, 5, 0, 0, 0, 6});
    std::vector<std::byte> value(incoming);
    value.insert(value.end(), response.begin(), response.end());
    const auto parameters = Tlv::parseAll(value);
    ASSERT_TRUE(parameters);
    ASSERT_EQ(parameters->size(), 2U);

    const auto request = IncomingResetRequest::parse((*parameters)[0]);
    ASSERT_TRUE(request);
    EXPECT_EQ(request->request_sequence, 5U);
    EXPECT_EQ(request->streams, (std::vector<std::uint16_t>{1, 3}));

    const auto parsed_response = ReconfigResponse::parse((*parameters)[1]);
    ASSERT_TRUE(parsed_response);
    EXPECT_EQ(parsed_response->response_sequence, 5U);
    EXPECT_EQ(parsed_response->result, ReconfigResult::InProgress);
}

TEST(ReconfigChunk, RejectsMalformedParameters)
{
    const auto odd_streams_bytes = bytes({0x00, 0x0d, 0x00, 0x11, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 7});
    const auto odd_streams = Tlv::parseAll(odd_streams_bytes);
    ASSERT_TRUE(odd_streams);
    EXPECT_FALSE(OutgoingResetRequest::parse((*odd_streams)[0]));

    const auto short_request_bytes = bytes({0x00, 0x0d, 0x00, 0x0c, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto short_request = Tlv::parseAll(short_request_bytes);
    ASSERT_TRUE(short_request);
    EXPECT_FALSE(OutgoingResetRequest::parse((*short_request)[0]));

    const auto unknown_result_bytes = bytes({0x00, 0x10, 0x00, 0x0c, 0, 0, 0, 1, 0, 0, 0, 7});
    const auto unknown_result = Tlv::parseAll(unknown_result_bytes);
    ASSERT_TRUE(unknown_result);
    EXPECT_FALSE(ReconfigResponse::parse((*unknown_result)[0]));

    const auto odd_response_bytes = bytes({0x00, 0x10, 0x00, 0x10, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0});
    const auto odd_response = Tlv::parseAll(odd_response_bytes);
    ASSERT_TRUE(odd_response);
    EXPECT_FALSE(ReconfigResponse::parse((*odd_response)[0]));
}

TEST(HeartbeatChunk, ParsesCapturedInfo)
{
    const auto chunk = chunkOf(CAPTURED_HEARTBEAT);
    const auto parameters = Tlv::parseAll(chunk.value);
    ASSERT_TRUE(parameters);
    ASSERT_EQ(parameters->size(), 1U);
    EXPECT_EQ((*parameters)[0].type, static_cast<std::uint16_t>(ParameterType::HeartbeatInfo));
    EXPECT_EQ((*parameters)[0].value.size(), 36U);
}

TEST(HeartbeatChunk, EchoesInfoUnchanged)
{
    const auto chunk = chunkOf(CAPTURED_HEARTBEAT);
    auto expected = CAPTURED_HEARTBEAT;
    expected[0] = std::byte{static_cast<std::uint8_t>(ChunkType::HeartbeatAck)};
    EXPECT_EQ(serialized([&](PacketBuilder &builder) { builder.addChunk(ChunkType::HeartbeatAck, 0, chunk.value); }),
              expected);
}

TEST(ControlChunks, SerializeCaptures)
{
    std::vector<std::byte> user_abort;
    ByteWriter(user_abort).writeTlv(static_cast<std::uint16_t>(CauseCode::UserInitiatedAbort), {});
    EXPECT_EQ(serialized([&](PacketBuilder &builder) { builder.addChunk(ChunkType::Abort, 0, user_abort); }),
              bytes({0x06, 0x00, 0x00, 0x08, 0x00, 0x0c, 0x00, 0x04}));

    std::vector<std::byte> unrecognized;
    ByteWriter(unrecognized)
        .writeTlv(static_cast<std::uint16_t>(CauseCode::UnrecognizedChunkType), bytes({0x49, 0x00, 0x00, 0x04}));
    EXPECT_EQ(serialized([&](PacketBuilder &builder) { builder.addChunk(ChunkType::Error, 0, unrecognized); }),
              bytes({0x09, 0x00, 0x00, 0x0c, 0x00, 0x06, 0x00, 0x08, 0x49, 0x00, 0x00, 0x04}));

    EXPECT_EQ(serialized([](PacketBuilder &builder) {
                  builder.addChunk(ChunkType::CookieEcho, 0, bytes({0x12, 0x34, 0x56, 0x78}));
              }),
              bytes({0x0a, 0x00, 0x00, 0x08, 0x12, 0x34, 0x56, 0x78}));

    EXPECT_EQ(serialized([](PacketBuilder &builder) {
                  builder.addChunk(ChunkType::Shutdown, 0, bytes({0x06, 0x11, 0xd1, 0xbd}));
              }),
              bytes({0x07, 0x00, 0x00, 0x08, 0x06, 0x11, 0xd1, 0xbd}));

    EXPECT_EQ(serialized([](PacketBuilder &builder) { builder.addChunk(ChunkType::CookieAck, 0, {}); }),
              bytes({0x0b, 0x00, 0x00, 0x04}));
    EXPECT_EQ(serialized([](PacketBuilder &builder) { builder.addChunk(ChunkType::ShutdownAck, 0, {}); }),
              bytes({0x08, 0x00, 0x00, 0x04}));
    EXPECT_EQ(serialized([](PacketBuilder &builder) { builder.addChunk(ChunkType::ShutdownComplete, 1, {}); }),
              bytes({0x0e, 0x01, 0x00, 0x04}));
}

TEST(Tlv, ParsesCausesWithPadding)
{
    const auto cause_bytes = bytes({0x00, 0x0d, 0x00, 0x05, 'x', 0, 0, 0, 0x00, 0x03, 0x00, 0x08, 0, 0, 0, 9});
    const auto causes = Tlv::parseAll(cause_bytes);
    ASSERT_TRUE(causes);
    ASSERT_EQ(causes->size(), 2U);
    EXPECT_EQ((*causes)[0].type, static_cast<std::uint16_t>(CauseCode::ProtocolViolation));
    EXPECT_EQ(copy((*causes)[0].value), bytes({'x'}));
    EXPECT_EQ((*causes)[1].type, static_cast<std::uint16_t>(CauseCode::StaleCookie));
}

TEST(Tlv, RejectsTruncatedHeader)
{
    EXPECT_FALSE(Tlv::parseAll(bytes({0x00, 0x0d, 0x00})));
}

}  // namespace
}  // namespace endgate::rtc::sctp
