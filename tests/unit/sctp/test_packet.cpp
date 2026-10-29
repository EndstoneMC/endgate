#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "bytes.h"
#include "endgate/network/rtc/sctp/crc32c.h"
#include "endgate/network/rtc/sctp/packet.h"

namespace endgate::rtc::sctp {
namespace {

const std::vector<std::byte> CAPTURED_INIT_PACKET = bytes({
    0x13, 0x88, 0x13, 0x88, 0x00, 0x00, 0x00, 0x00, 0xaa, 0x01, 0x9d, 0x33, 0x01, 0x00, 0x00, 0x5a, 0x0e, 0xdd,
    0xca, 0x08, 0x00, 0x02, 0x00, 0x00, 0x03, 0xe8, 0x03, 0xe8, 0x55, 0x08, 0x36, 0x37, 0x80, 0x00, 0x00, 0x04,
    0xc0, 0x00, 0x00, 0x04, 0x80, 0x08, 0x00, 0x09, 0xc0, 0x0f, 0xc1, 0x80, 0x82, 0x00, 0x00, 0x00, 0x80, 0x02,
    0x00, 0x24, 0xc5, 0xa8, 0x61, 0x55, 0x09, 0x0e, 0x6f, 0x42, 0x00, 0x50, 0x63, 0x4c, 0xc8, 0xd6, 0xb9, 0x08,
    0xdf, 0xd5, 0x3e, 0x17, 0xc9, 0x9c, 0xb1, 0x43, 0x28, 0x4e, 0xaf, 0x64, 0x68, 0x2a, 0xc2, 0x97, 0x80, 0x04,
    0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x80, 0x03, 0x00, 0x06, 0x80, 0xc1, 0x00, 0x00,
});

const std::vector<std::byte> CAPTURED_COOKIE_ACK_AND_SACK = bytes({
    0x04, 0xd2, 0x10, 0xe1, 0x69, 0x7e, 0x3a, 0x4e, 0xc0, 0x6e, 0x8b, 0x36, 0x0b, 0x00, 0x00, 0x04,
    0x03, 0x00, 0x00, 0x10, 0xae, 0xa9, 0x52, 0x52, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
});

std::vector<std::byte> sealed(std::vector<std::byte> packet)
{
    std::fill_n(packet.begin() + 8, 4, std::byte{0});
    const auto checksum = crc32c(packet);
    for (std::size_t i = 0; i < 4; ++i) {
        packet[8 + i] = static_cast<std::byte>(checksum >> (8 * i));
    }
    return packet;
}

TEST(Packet, ParsesCapturedInit)
{
    const auto packet = Packet::parse(CAPTURED_INIT_PACKET);
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->header.source_port, 5000);
    EXPECT_EQ(packet->header.destination_port, 5000);
    EXPECT_EQ(packet->header.verification_tag, 0U);
    ASSERT_EQ(packet->chunks.size(), 1U);
    EXPECT_EQ(packet->chunks[0].type, static_cast<std::uint8_t>(ChunkType::Init));
    EXPECT_EQ(packet->chunks[0].value.size(), 0x5aU - 4);
}

TEST(Packet, ParsesCapturedBundle)
{
    const auto packet = Packet::parse(CAPTURED_COOKIE_ACK_AND_SACK);
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->header.source_port, 1234);
    EXPECT_EQ(packet->header.destination_port, 4321);
    EXPECT_EQ(packet->header.verification_tag, 0x697e3a4eU);
    ASSERT_EQ(packet->chunks.size(), 2U);
    EXPECT_EQ(packet->chunks[0].type, static_cast<std::uint8_t>(ChunkType::CookieAck));
    EXPECT_TRUE(packet->chunks[0].value.empty());
    EXPECT_EQ(packet->chunks[1].type, static_cast<std::uint8_t>(ChunkType::Sack));
    EXPECT_EQ(packet->chunks[1].value.size(), 12U);
}

TEST(Packet, RejectsWrongChecksum)
{
    auto packet = CAPTURED_INIT_PACKET;
    packet[11] ^= std::byte{1};
    EXPECT_FALSE(Packet::parse(packet));

    const auto sack = bytes({0x13, 0x88, 0x13, 0x88, 0x0e, 0xdd, 0xca, 0x08, 0x2a, 0x81, 0xf5, 0x31, 0x03, 0x00,
                             0x00, 0x10, 0x55, 0x08, 0x36, 0x40, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_FALSE(Packet::parse(sack));
}

TEST(Packet, RejectsCorruptedPayloadByte)
{
    for (std::size_t i = 0; i < CAPTURED_COOKIE_ACK_AND_SACK.size(); ++i) {
        if (i >= 8 && i < 12) {
            continue;
        }
        auto packet = CAPTURED_COOKIE_ACK_AND_SACK;
        packet[i] ^= std::byte{0x40};
        EXPECT_FALSE(Packet::parse(packet)) << i;
    }
}

TEST(Packet, RejectsGarbage)
{
    EXPECT_FALSE(Packet::parse(
        bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0x0a, 0x0a, 0x0a, 0x5c, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x00, 0x00, 0x00})));
    EXPECT_FALSE(Packet::parse({}));
    EXPECT_FALSE(Packet::parse(std::span{CAPTURED_INIT_PACKET}.first(15)));
}

TEST(Packet, RejectsHeaderWithoutChunks)
{
    EXPECT_FALSE(Packet::parse(sealed(bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0}))));
}

TEST(Packet, RejectsChunkShorterThanItsHeader)
{
    EXPECT_FALSE(
        Packet::parse(sealed(bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0, 0x0b, 0x00, 0x00, 0x03}))));
}

TEST(Packet, RejectsChunkLongerThanPacket)
{
    EXPECT_FALSE(
        Packet::parse(sealed(bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0, 0x0b, 0x00, 0x00, 0x08}))));
}

TEST(Packet, RejectsTruncatedChunkHeader)
{
    EXPECT_FALSE(Packet::parse(
        sealed(bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0, 0x0b, 0x00, 0x00, 0x04, 0x0b, 0x00}))));
}

TEST(Packet, AcceptsUnpaddedLastChunk)
{
    const auto packet = Packet::parse(sealed(
        bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0, 0x0b, 0x00, 0x00, 0x04, 0x0a, 0x00, 0x00, 0x05, 0x2a})));
    ASSERT_TRUE(packet);
    ASSERT_EQ(packet->chunks.size(), 2U);
    EXPECT_EQ(packet->chunks[1].value.size(), 1U);
}

TEST(Packet, ExposesUnknownChunkTypes)
{
    const auto packet = Packet::parse(sealed(
        bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0, 0x49, 0x05, 0x00, 0x06, 0x01, 0x02, 0x00, 0x00})));
    ASSERT_TRUE(packet);
    ASSERT_EQ(packet->chunks.size(), 1U);
    EXPECT_EQ(packet->chunks[0].type, 0x49);
    EXPECT_EQ(packet->chunks[0].flags, 0x05);
    EXPECT_EQ(packet->chunks[0].value.size(), 2U);
}

TEST(PacketBuilder, ReproducesCapturedInitByteForByte)
{
    const auto parsed = Packet::parse(CAPTURED_INIT_PACKET);
    ASSERT_TRUE(parsed);
    const auto init = InitChunk::parse(parsed->chunks[0]);
    ASSERT_TRUE(init);

    std::vector<std::byte> buffer;
    PacketBuilder builder(buffer, parsed->header, 1200);
    builder.addInit(ChunkType::Init, *init, parsed->chunks[0].value.subspan(16));
    const auto packet = builder.build();
    EXPECT_EQ(std::vector<std::byte>(packet.begin(), packet.end()), CAPTURED_INIT_PACKET);
}

TEST(PacketBuilder, ReproducesCapturedBundleByteForByte)
{
    std::vector<std::byte> buffer;
    PacketBuilder builder(buffer, {.source_port = 1234, .destination_port = 4321, .verification_tag = 0x697e3a4e},
                          1200);
    builder.addChunk(ChunkType::CookieAck, 0, {});
    builder.addSack({.cumulative_tsn_ack = 0xaea95252, .a_rwnd = 131072, .gap_blocks = {}, .duplicate_tsns = {}});
    const auto packet = builder.build();
    EXPECT_EQ(std::vector<std::byte>(packet.begin(), packet.end()), CAPTURED_COOKIE_ACK_AND_SACK);
}

TEST(PacketBuilder, ReportsSpaceWithinMtu)
{
    std::vector<std::byte> buffer;
    PacketBuilder builder(buffer, {.source_port = 5000, .destination_port = 5000, .verification_tag = 1}, 1191);
    EXPECT_TRUE(builder.empty());
    EXPECT_EQ(builder.remaining(), 1176U);

    builder.addData({.tsn = 1, .stream = 0, .ssn = 0, .ppid = 53, .flags = 3, .payload = {}},
                    LegacyByteBuf::copyBuffer(std::vector<std::byte>(183)));
    EXPECT_FALSE(builder.empty());
    EXPECT_EQ(builder.remaining(), 1176U - 16 - 184);

    builder.addData({.tsn = 2, .stream = 0, .ssn = 1, .ppid = 53, .flags = 3, .payload = {}},
                    LegacyByteBuf::copyBuffer(std::vector<std::byte>(1176 - 16 - 184 - 16)));
    EXPECT_EQ(builder.remaining(), 0U);
    EXPECT_EQ(builder.build().size(), 1188U);
}

TEST(PacketBuilder, RoundTripsThroughParse)
{
    std::vector<std::byte> buffer;
    PacketBuilder builder(buffer, {.source_port = 5000, .destination_port = 5001, .verification_tag = 0xdeadbeef},
                          1200);
    builder.addChunk(ChunkType::Heartbeat, 0, bytes({0x00, 0x01, 0x00, 0x05, 0x7f}));
    builder.addData({.tsn = 9, .stream = 3, .ssn = 4, .ppid = 51, .flags = 7, .payload = {}},
                    LegacyByteBuf::copyBuffer(bytes({1, 2, 3})));
    const auto packet = Packet::parse(builder.build());
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->header.source_port, 5000);
    EXPECT_EQ(packet->header.destination_port, 5001);
    EXPECT_EQ(packet->header.verification_tag, 0xdeadbeefU);
    ASSERT_EQ(packet->chunks.size(), 2U);
    EXPECT_EQ(packet->chunks[0].value.size(), 5U);
    const auto data = DataChunk::parse(packet->chunks[1]);
    ASSERT_TRUE(data);
    EXPECT_EQ(data->tsn, 9U);
    EXPECT_EQ(data->stream, 3);
    EXPECT_EQ(data->ssn, 4);
    EXPECT_EQ(data->ppid, 51U);
    EXPECT_EQ(data->flags, 7);
    EXPECT_EQ(std::vector<std::byte>(data->payload.begin(), data->payload.end()), bytes({1, 2, 3}));
}

}  // namespace
}  // namespace endgate::rtc::sctp
