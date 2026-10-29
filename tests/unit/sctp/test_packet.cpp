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
    const auto packet = Packet::parse(Unpooled::wrappedBuffer(CAPTURED_INIT_PACKET));
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->header.source_port, 5000);
    EXPECT_EQ(packet->header.destination_port, 5000);
    EXPECT_EQ(packet->header.verification_tag, 0U);
    ASSERT_EQ(packet->chunks.size(), 1U);
    EXPECT_EQ(packet->chunks[0].type, static_cast<std::uint8_t>(ChunkType::Init));
    EXPECT_EQ(packet->chunks[0].value.readableBytes(), 0x5aU - 4);
}

TEST(Packet, ParsesCapturedBundle)
{
    const auto packet = Packet::parse(Unpooled::wrappedBuffer(CAPTURED_COOKIE_ACK_AND_SACK));
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->header.source_port, 1234);
    EXPECT_EQ(packet->header.destination_port, 4321);
    EXPECT_EQ(packet->header.verification_tag, 0x697e3a4eU);
    ASSERT_EQ(packet->chunks.size(), 2U);
    EXPECT_EQ(packet->chunks[0].type, static_cast<std::uint8_t>(ChunkType::CookieAck));
    EXPECT_FALSE(packet->chunks[0].value.isReadable());
    EXPECT_EQ(packet->chunks[1].type, static_cast<std::uint8_t>(ChunkType::Sack));
    EXPECT_EQ(packet->chunks[1].value.readableBytes(), 12U);
}

TEST(Packet, RejectsWrongChecksum)
{
    auto packet = CAPTURED_INIT_PACKET;
    packet[11] ^= std::byte{1};
    EXPECT_FALSE(Packet::parse(Unpooled::wrappedBuffer(packet)));

    const auto sack = bytes({0x13, 0x88, 0x13, 0x88, 0x0e, 0xdd, 0xca, 0x08, 0x2a, 0x81, 0xf5, 0x31, 0x03, 0x00,
                             0x00, 0x10, 0x55, 0x08, 0x36, 0x40, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_FALSE(Packet::parse(Unpooled::wrappedBuffer(sack)));
}

TEST(Packet, RejectsCorruptedPayloadByte)
{
    for (std::size_t i = 0; i < CAPTURED_COOKIE_ACK_AND_SACK.size(); ++i) {
        if (i >= 8 && i < 12) {
            continue;
        }
        auto packet = CAPTURED_COOKIE_ACK_AND_SACK;
        packet[i] ^= std::byte{0x40};
        EXPECT_FALSE(Packet::parse(Unpooled::wrappedBuffer(packet))) << i;
    }
}

TEST(Packet, RejectsGarbage)
{
    EXPECT_FALSE(Packet::parse(Unpooled::wrappedBuffer(bytes(
        {0xff, 0xff, 0xff, 0xff, 0xff, 0x0a, 0x0a, 0x0a, 0x5c, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x00, 0x00, 0x00}))));
    EXPECT_FALSE(Packet::parse(ByteBufView{}));
    EXPECT_FALSE(Packet::parse(Unpooled::wrappedBuffer(std::span{CAPTURED_INIT_PACKET}.first(15))));
}

TEST(Packet, RejectsHeaderWithoutChunks)
{
    EXPECT_FALSE(
        Packet::parse(Unpooled::wrappedBuffer(sealed(bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0})))));
}

TEST(Packet, RejectsChunkShorterThanItsHeader)
{
    EXPECT_FALSE(Packet::parse(Unpooled::wrappedBuffer(
        sealed(bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0, 0x0b, 0x00, 0x00, 0x03})))));
}

TEST(Packet, RejectsChunkLongerThanPacket)
{
    EXPECT_FALSE(Packet::parse(Unpooled::wrappedBuffer(
        sealed(bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0, 0x0b, 0x00, 0x00, 0x08})))));
}

TEST(Packet, RejectsTruncatedChunkHeader)
{
    EXPECT_FALSE(Packet::parse(Unpooled::wrappedBuffer(
        sealed(bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0, 0x0b, 0x00, 0x00, 0x04, 0x0b, 0x00})))));
}

TEST(Packet, AcceptsUnpaddedLastChunk)
{
    const auto raw = sealed(
        bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0, 0x0b, 0x00, 0x00, 0x04, 0x0a, 0x00, 0x00, 0x05, 0x2a}));
    const auto packet = Packet::parse(Unpooled::wrappedBuffer(raw));
    ASSERT_TRUE(packet);
    ASSERT_EQ(packet->chunks.size(), 2U);
    EXPECT_EQ(packet->chunks[1].value.readableBytes(), 1U);
}

TEST(Packet, ExposesUnknownChunkTypes)
{
    const auto raw =
        sealed(bytes({0x13, 0x88, 0x13, 0x88, 0, 0, 0, 1, 0, 0, 0, 0, 0x49, 0x05, 0x00, 0x06, 0x01, 0x02, 0x00, 0x00}));
    const auto packet = Packet::parse(Unpooled::wrappedBuffer(raw));
    ASSERT_TRUE(packet);
    ASSERT_EQ(packet->chunks.size(), 1U);
    EXPECT_EQ(packet->chunks[0].type, 0x49);
    EXPECT_EQ(packet->chunks[0].flags, 0x05);
    EXPECT_EQ(packet->chunks[0].value.readableBytes(), 2U);
}

TEST(PacketWriter, ReproducesCapturedInitByteForByte)
{
    const auto parsed = Packet::parse(Unpooled::wrappedBuffer(CAPTURED_INIT_PACKET));
    ASSERT_TRUE(parsed);
    const auto init = InitChunk::parse(parsed->chunks[0]);
    ASSERT_TRUE(init);

    auto packet = Unpooled::buffer(1200);
    parsed->header.write(packet);
    init->write(packet, ChunkType::Init,
                parsed->chunks[0].value.slice(16, parsed->chunks[0].value.readableBytes() - 16));
    Packet::seal(packet);
    EXPECT_EQ(bytesOf(packet), CAPTURED_INIT_PACKET);
}

TEST(PacketWriter, ReproducesCapturedBundleByteForByte)
{
    auto packet = Unpooled::buffer(1200);
    CommonHeader{.source_port = 1234, .destination_port = 4321, .verification_tag = 0x697e3a4e}.write(packet);
    Chunk{.type = static_cast<std::uint8_t>(ChunkType::CookieAck), .flags = 0, .value = {}}.write(packet);
    SackChunk{.cumulative_tsn_ack = 0xaea95252, .a_rwnd = 131072, .gap_blocks = {}, .duplicate_tsns = {}}.write(packet);
    Packet::seal(packet);
    EXPECT_EQ(bytesOf(packet), CAPTURED_COOKIE_ACK_AND_SACK);
}

TEST(PacketWriter, ReportsSpaceWithinMtu)
{
    auto packet = Unpooled::buffer(1188, 1188);
    CommonHeader{.source_port = 5000, .destination_port = 5000, .verification_tag = 1}.write(packet);
    EXPECT_EQ(packet.writerIndex(), CommonHeader::SIZE);
    EXPECT_EQ(packet.maxWritableBytes(), 1176U);

    const std::vector<std::byte> first(183);
    DataChunk{.tsn = 1, .stream = 0, .ssn = 0, .ppid = 53, .flags = 3, .payload = Unpooled::wrappedBuffer(first)}.write(
        packet);
    EXPECT_NE(packet.writerIndex(), CommonHeader::SIZE);
    EXPECT_EQ(packet.maxWritableBytes(), 1176U - 16 - 184);

    const std::vector<std::byte> second(1176 - 16 - 184 - 16);
    DataChunk{.tsn = 2, .stream = 0, .ssn = 1, .ppid = 53, .flags = 3, .payload = Unpooled::wrappedBuffer(second)}
        .write(packet);
    EXPECT_EQ(packet.maxWritableBytes(), 0U);
    Packet::seal(packet);
    EXPECT_EQ(packet.readableBytes(), 1188U);
}

TEST(PacketWriter, RoundTripsThroughParse)
{
    auto buffer = Unpooled::buffer();
    CommonHeader{.source_port = 5000, .destination_port = 5001, .verification_tag = 0xdeadbeef}.write(buffer);
    Chunk{.type = static_cast<std::uint8_t>(ChunkType::Heartbeat),
          .flags = 0,
          .value = Unpooled::wrappedBuffer(bytes({0x00, 0x01, 0x00, 0x05, 0x7f}))}
        .write(buffer);
    DataChunk{
        .tsn = 9, .stream = 3, .ssn = 4, .ppid = 51, .flags = 7, .payload = Unpooled::wrappedBuffer(bytes({1, 2, 3}))}
        .write(buffer);
    Packet::seal(buffer);
    const auto packet = Packet::parse(buffer.slice());
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->header.source_port, 5000);
    EXPECT_EQ(packet->header.destination_port, 5001);
    EXPECT_EQ(packet->header.verification_tag, 0xdeadbeefU);
    ASSERT_EQ(packet->chunks.size(), 2U);
    EXPECT_EQ(packet->chunks[0].value.readableBytes(), 5U);
    const auto data = DataChunk::parse(packet->chunks[1]);
    ASSERT_TRUE(data);
    EXPECT_EQ(data->tsn, 9U);
    EXPECT_EQ(data->stream, 3);
    EXPECT_EQ(data->ssn, 4);
    EXPECT_EQ(data->ppid, 51U);
    EXPECT_EQ(data->flags, 7);
    EXPECT_EQ(bytesOf(data->payload), bytes({1, 2, 3}));
}

}  // namespace
}  // namespace endgate::rtc::sctp
