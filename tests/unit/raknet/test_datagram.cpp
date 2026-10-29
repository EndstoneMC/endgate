#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/network/raknet/datagram.h"
#include "support.h"

namespace endgate::raknet {
namespace {

using test::fromHex;
using test::toHex;

const auto PAYLOAD = fromHex("fe0102");

std::string encodeFrame(const FrameHeader &header)
{
    std::array<std::byte, 64> buffer{};
    Writer writer{buffer};
    header.encode(writer, PAYLOAD.size());
    writer.writeBytes(PAYLOAD);
    EXPECT_EQ(writer.size(), header.length() + PAYLOAD.size());
    return toHex(writer.written());
}

std::string encodeHeader(const DatagramHeader &header)
{
    std::array<std::byte, 16> buffer{};
    Writer writer{buffer};
    header.encode(writer);
    return toHex(writer.written());
}

struct GoldenFrame {
    FrameHeader header;
    std::string_view legacy;
};

const std::array<GoldenFrame, 8> GOLDEN_FRAMES{{
    {.header = {.reliability = PacketReliability::Unreliable}, .legacy = "000018fe0102"},
    {.header = {.reliability = PacketReliability::UnreliableSequenced,
                .reliable_message_number = UINT24_MASK,
                .sequencing_index = 0x010203,
                .ordering_index = 0x040506,
                .ordering_channel = 7},
     .legacy = "20001803020106050407fe0102"},
    {.header = {.reliability = PacketReliability::Reliable, .reliable_message_number = 0x0A0B0C},
     .legacy = "4000180c0b0afe0102"},
    {.header = {.reliability = PacketReliability::ReliableOrdered,
                .reliable_message_number = 0x0A0B0C,
                .sequencing_index = 0,
                .ordering_index = 0x040506,
                .ordering_channel = 31},
     .legacy = "6000180c0b0a0605041ffe0102"},
    {.header = {.reliability = PacketReliability::ReliableSequenced,
                .reliable_message_number = 0x0A0B0C,
                .sequencing_index = 0x010203,
                .ordering_index = 0x040506,
                .ordering_channel = 3},
     .legacy = "8000180c0b0a03020106050403fe0102"},
    {.header = {.reliability = PacketReliability::ReliableOrdered,
                .reliable_message_number = 0x000102,
                .sequencing_index = 0,
                .ordering_index = 0x000009,
                .ordering_channel = 0,
                .split_packet_count = 5,
                .split_packet_id = 0x1234,
                .split_packet_index = 4},
     .legacy = "7000180201000900000000000005123400000004fe0102"},
    {.header = {.reliability = PacketReliability::ReliableOrderedWithAckReceipt,
                .reliable_message_number = 0x0A0B0C,
                .sequencing_index = 0,
                .ordering_index = 0x040506,
                .ordering_channel = 2},
     .legacy = "6000180c0b0a06050402fe0102"},
    {.header = {.reliability = PacketReliability::UnreliableWithAckReceipt}, .legacy = "000018fe0102"},
}};

TEST(Datagram, FramesMatchLegacyPort)
{
    for (const auto &frame : GOLDEN_FRAMES) {
        EXPECT_EQ(encodeFrame(frame.header), frame.legacy);
    }
}

TEST(Datagram, FramesMatchWirePy)
{
    EXPECT_EQ(encodeFrame(GOLDEN_FRAMES[0].header), "000018fe0102");
    EXPECT_EQ(encodeFrame(GOLDEN_FRAMES[1].header), "20001803020106050407fe0102");
    EXPECT_EQ(encodeFrame(GOLDEN_FRAMES[2].header), "4000180c0b0afe0102");
    EXPECT_EQ(encodeFrame(GOLDEN_FRAMES[3].header), "6000180c0b0a0605041ffe0102");
    EXPECT_EQ(encodeFrame(GOLDEN_FRAMES[4].header), "8000180c0b0a03020106050403fe0102");
    EXPECT_EQ(encodeFrame(GOLDEN_FRAMES[5].header), "7000180201000900000000000005123400000004fe0102");
}

TEST(Datagram, FramesRoundTrip)
{
    for (const auto &frame : std::span{GOLDEN_FRAMES}.first(6)) {
        const auto bytes = fromHex(frame.legacy);
        Reader reader{bytes};
        const auto decoded = Frame::decode(reader);
        ASSERT_TRUE(decoded) << frame.legacy;
        EXPECT_EQ(decoded->header.reliability, frame.header.reliability);
        EXPECT_EQ(decoded->header.reliable_message_number, frame.header.reliable_message_number);
        EXPECT_EQ(decoded->header.sequencing_index, frame.header.sequencing_index);
        EXPECT_EQ(decoded->header.ordering_index, frame.header.ordering_index);
        EXPECT_EQ(decoded->header.ordering_channel, frame.header.ordering_channel);
        EXPECT_EQ(decoded->header.split_packet_count, frame.header.split_packet_count);
        EXPECT_EQ(decoded->header.split_packet_id, frame.header.split_packet_id);
        EXPECT_EQ(decoded->header.split_packet_index, frame.header.split_packet_index);
        EXPECT_EQ(toHex(decoded->data), "fe0102");
        EXPECT_EQ(reader.remaining(), 0U);
    }
}

TEST(Datagram, ReceiptReliabilitiesOnTheWireDecodeAsTheReferenceDoes)
{
    const auto unreliable_receipt = fromHex("a00010fe01");
    Reader first{unreliable_receipt};
    const auto five = Frame::decode(first);
    ASSERT_TRUE(five);
    EXPECT_EQ(five->header.reliability, PacketReliability::UnreliableWithAckReceipt);
    EXPECT_EQ(toHex(five->data), "fe01");

    const auto reliable_receipt = fromHex("c000100c0b0afe01");
    Reader second{reliable_receipt};
    const auto six = Frame::decode(second);
    ASSERT_TRUE(six);
    EXPECT_EQ(six->header.reliable_message_number, UINT24_MASK);
    EXPECT_EQ(toHex(six->data), "0c0b");

    const auto ordered_receipt = fromHex("e00010060504"
                                         "02fe01");
    Reader third{ordered_receipt};
    const auto seven = Frame::decode(third);
    ASSERT_TRUE(seven);
    EXPECT_EQ(seven->header.ordering_index, 0x040506U);
    EXPECT_EQ(seven->header.ordering_channel, 2);
    EXPECT_EQ(toHex(seven->data), "fe01");
}

TEST(Datagram, PartialBitLengthsRoundUp)
{
    const auto frame = fromHex("000009"
                               "1801");
    Reader reader{frame};
    const auto decoded = Frame::decode(reader);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(toHex(decoded->data), "1801");
}

TEST(Datagram, MalformedFramesAreRejected)
{
    for (const auto *hex : {
             "0000",
             "000000fe",
             "60001800000000"
             "20"
             "fe0102",
             "50001800000000"
             "00000002"
             "0000"
             "00000002"
             "fe0102",
             "50001800000000"
             "00000000"
             "0000"
             "00000000"
             "fe0102",
             "400018000000"
             "fe01",
             "000018fe01",
             "6000180c0b0a060504",
             "500018000000"
             "000000020000",
         }) {
        const auto bytes = fromHex(hex);
        Reader reader{bytes};
        EXPECT_FALSE(Frame::decode(reader)) << hex;
    }
}

TEST(Datagram, LargestFrameHeaderIsReliableSequencedAndSplit)
{
    const FrameHeader header{.reliability = PacketReliability::ReliableSequenced,
                             .reliable_message_number = 0,
                             .sequencing_index = 0,
                             .ordering_index = 0,
                             .ordering_channel = 0,
                             .split_packet_count = 1};
    EXPECT_EQ(header.length(), MAXIMUM_FRAME_HEADER_LENGTH);
    EXPECT_EQ(MAXIMUM_FRAME_HEADER_LENGTH, 23U);
}

TEST(Datagram, DataHeaderMatchesLegacyPortAndWirePy)
{
    EXPECT_EQ(encodeHeader({.is_continuous_send = true, .needs_b_and_as = true, .datagram_number = 0x123456}),
              "8c563412");
    EXPECT_EQ(encodeHeader({.needs_b_and_as = true, .datagram_number = 0x123456}), "84563412");
    EXPECT_EQ(encodeHeader({.is_packet_pair = true, .datagram_number = 1}), "90010000");
    EXPECT_EQ(DatagramHeader::DATA_HEADER_BYTE_LENGTH, 9U);
}

TEST(Datagram, AckAndNakHeadersMatchLegacyPort)
{
    EXPECT_EQ(encodeHeader({.is_ack = true}), "c0");
    EXPECT_EQ(encodeHeader({.is_ack = true, .has_b_and_as = true, .as = 1.5F}), "e03fc00000");
    EXPECT_EQ(encodeHeader({.is_nak = true}), "a0");
}

TEST(Datagram, HeadersDecode)
{
    const auto data = fromHex("8c563412");
    Reader data_reader{data};
    const auto decoded = DatagramHeader::decode(data_reader);
    ASSERT_TRUE(decoded);
    EXPECT_FALSE(decoded->is_ack);
    EXPECT_FALSE(decoded->is_nak);
    EXPECT_TRUE(decoded->is_continuous_send);
    EXPECT_TRUE(decoded->needs_b_and_as);
    EXPECT_EQ(decoded->datagram_number, 0x123456U);

    const auto ack = fromHex("e03fc00000");
    Reader ack_reader{ack};
    const auto decoded_ack = DatagramHeader::decode(ack_reader);
    ASSERT_TRUE(decoded_ack);
    EXPECT_TRUE(decoded_ack->is_ack);
    EXPECT_EQ(decoded_ack->as, 1.5F);

    const auto nak = fromHex("a0");
    Reader nak_reader{nak};
    EXPECT_TRUE(DatagramHeader::decode(nak_reader)->is_nak);

    for (const auto *hex : {"40000000", "8c5634", "e03fc0"}) {
        const auto bytes = fromHex(hex);
        Reader reader{bytes};
        EXPECT_FALSE(DatagramHeader::decode(reader)) << hex;
    }
}

std::string serialize(RangeList &list, std::size_t max_bits)
{
    std::array<std::byte, MAXIMUM_MTU_SIZE> buffer{};
    Writer writer{buffer};
    list.serialize(writer, max_bits);
    return toHex(writer.written());
}

TEST(Datagram, AckRangesMatchLegacyPortAndWirePy)
{
    RangeList acks;
    for (const std::uint32_t number : {1U, 2U, 3U, 7U, 9U, 10U, 0x123456U}) {
        acks.insert(number);
    }
    EXPECT_EQ(serialize(acks, (1400 - 28 - 9) * 8), "00040001000003000001070000000900000a000001563412");
    EXPECT_TRUE(acks.empty());

    RangeList naks;
    naks.insert(5);
    EXPECT_EQ(serialize(naks, (1400 - 28 - 9) * 8), "0001"
                                                    "01050000");
}

TEST(Datagram, RangesThatDoNotFitStayForTheNextDatagram)
{
    RangeList acks;
    for (std::uint32_t number = 0; number < 2000; number += 2) {
        acks.insert(number);
    }
    EXPECT_EQ(fromHex(serialize(acks, (576 - 28 - 9) * 8)).size(), 426U);
    EXPECT_EQ(acks.ranges().size(), 894U);
    EXPECT_EQ(acks.ranges().front().min, 212U);
}

TEST(Datagram, RangeInsertMergesNeighbours)
{
    RangeList list;
    for (const std::uint32_t number : {5U, 7U, 6U, 3U, 4U, 9U, 1U, 7U, 4U}) {
        list.insert(number);
    }
    EXPECT_EQ(list.ranges(), (std::vector<Range>{{1, 1}, {3, 7}, {9, 9}}));
    list.insert(2);
    list.insert(8);
    EXPECT_EQ(list.ranges(), (std::vector<Range>{{1, 9}}));
}

TEST(Datagram, RangesDeserialize)
{
    const auto bytes = fromHex("00040001000003000001070000000900000a000001563412");
    Reader reader{bytes};
    const auto ranges = RangeList::deserialize(reader);
    ASSERT_TRUE(ranges);
    EXPECT_EQ(*ranges, (std::vector<Range>{{1, 3}, {7, 7}, {9, 10}, {0x123456, 0x123456}}));
}

TEST(Datagram, MalformedRangesAreRejected)
{
    for (const auto *hex : {"", "00", "0001", "000100050000", "0001000500000200000", "000100050000020000"}) {
        const auto bytes = fromHex(hex);
        Reader reader{bytes};
        EXPECT_EQ(RangeList::deserialize(reader).error_or(RangeListError::TooManyRanges), RangeListError::Truncated)
            << hex;
    }
    const auto truncated_count = fromHex("0401"
                                         "01000000");
    Reader truncated_reader{truncated_count};
    EXPECT_EQ(RangeList::deserialize(truncated_reader).error(), RangeListError::Truncated);

    std::string too_many = "0401";
    for (std::uint32_t i = 0; i < 0x401; ++i) {
        too_many += "01" + toHex(std::array{std::byte(i * 2 & 0xFF), std::byte(i * 2 >> 8), std::byte{0}});
    }
    const auto too_many_bytes = fromHex(too_many);
    Reader reader{too_many_bytes};
    EXPECT_EQ(RangeList::deserialize(reader).error(), RangeListError::TooManyRanges);
}

}  // namespace
}  // namespace endgate::raknet
