#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

#include <gtest/gtest.h>

#include "endgate/buffer/bytebuf.h"

namespace endgate {
namespace {

std::array<std::byte, 4> bytes(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d)
{
    return {std::byte{a}, std::byte{b}, std::byte{c}, std::byte{d}};
}

TEST(ByteBuf, WritesBigEndianByDefaultAndLittleEndianWithLe)
{
    auto buf = Unpooled::buffer();
    buf.writeByte(0x01);
    buf.writeShort(0x0203);
    buf.writeShortLE(0x0405);
    buf.writeMedium(0x060708);
    buf.writeMediumLE(0x090A0B);
    buf.writeInt(0x0C0D0E0F);
    buf.writeIntLE(0x10111213);
    buf.writeLong(0x1415161718191A1B);
    buf.writeLongLE(0x1C1D1E1F20212223);
    EXPECT_EQ(ByteBufUtil::hexDump(buf), "01"
                                         "0203"
                                         "0504"
                                         "060708"
                                         "0b0a09"
                                         "0c0d0e0f"
                                         "13121110"
                                         "1415161718191a1b"
                                         "232221201f1e1d1c");
}

TEST(ByteBuf, ReadsWhatItWrote)
{
    auto buf = Unpooled::buffer();
    buf.writeBoolean(true);
    buf.writeByte(0xFF);
    buf.writeShort(0xFFFE);
    buf.writeShortLE(0x1234);
    buf.writeMedium(0x800000);
    buf.writeMediumLE(0x123456);
    buf.writeInt(0xFFFFFFFF);
    buf.writeIntLE(0x89ABCDEF);
    buf.writeLong(0xFFFFFFFFFFFFFFFF);
    buf.writeLongLE(0x0123456789ABCDEF);
    buf.writeFloat(1.5F);
    buf.writeFloatLE(-2.25F);
    buf.writeDouble(3.5);
    buf.writeDoubleLE(-0.125);

    EXPECT_TRUE(buf.readBoolean());
    EXPECT_EQ(buf.readByte(), -1);
    EXPECT_EQ(buf.readShort(), -2);
    EXPECT_EQ(buf.readUnsignedShortLE(), 0x1234);
    EXPECT_EQ(buf.readMedium(), -0x800000);
    EXPECT_EQ(buf.readUnsignedMediumLE(), 0x123456U);
    EXPECT_EQ(buf.readUnsignedInt(), 0xFFFFFFFFU);
    EXPECT_EQ(buf.readUnsignedIntLE(), 0x89ABCDEFU);
    EXPECT_EQ(buf.readLong(), -1);
    EXPECT_EQ(buf.readLongLE(), 0x0123456789ABCDEF);
    EXPECT_EQ(buf.readFloat(), 1.5F);
    EXPECT_EQ(buf.readFloatLE(), -2.25F);
    EXPECT_EQ(buf.readDouble(), 3.5);
    EXPECT_EQ(buf.readDoubleLE(), -0.125);
    EXPECT_FALSE(buf.isReadable());
}

TEST(ByteBuf, GetAndSetLeaveTheIndicesAlone)
{
    auto buf = Unpooled::buffer(8);
    buf.writeZero(8);
    buf.setIntLE(2, 0xAABBCCDD);
    EXPECT_EQ(buf.getUnsignedInt(2), 0xDDCCBBAAU);
    EXPECT_EQ(buf.readerIndex(), 0U);
    EXPECT_EQ(buf.writerIndex(), 8U);
}

TEST(ByteBuf, ReadingPastTheWriterIndexThrows)
{
    auto buf = Unpooled::buffer(8);
    buf.writeShort(1);
    EXPECT_THROW((void)buf.readInt(), std::out_of_range);
    EXPECT_EQ(buf.readerIndex(), 0U);
}

TEST(ByteBuf, GrowsByNettysRule)
{
    auto buf = Unpooled::buffer(0);
    buf.writeByte(1);
    EXPECT_EQ(buf.capacity(), 64U);
    buf.writeZero(64);
    EXPECT_EQ(buf.capacity(), 128U);
    buf.ensureWritable(4 * 1048576);
    EXPECT_EQ(buf.capacity(), 8U * 1048576);
}

TEST(ByteBuf, GrowingKeepsTheBytesAlreadyWritten)
{
    auto buf = Unpooled::buffer(2);
    buf.writeShort(0xBEEF);
    buf.writeInt(0x01020304);
    EXPECT_EQ(ByteBufUtil::hexDump(buf), "beef01020304");
}

TEST(ByteBuf, NeverGrowsPastItsMaxCapacity)
{
    auto buf = Unpooled::buffer(4, 6);
    buf.writeInt(0);
    EXPECT_THROW(buf.writeInt(0), std::out_of_range);
    buf.writeShort(0);
    EXPECT_EQ(buf.capacity(), 6U);
}

TEST(ByteBuf, ReadSliceBorrowsTheBytesAndAdvances)
{
    auto buf = Unpooled::copiedBuffer(bytes(1, 2, 3, 4));
    auto slice = buf.readSlice(3);
    EXPECT_EQ(buf.readerIndex(), 3U);
    EXPECT_EQ(slice.readableBytes(), 3U);
    buf.setByte(1, 9);
    EXPECT_EQ(slice.readUnsignedByte(), 1);
    EXPECT_EQ(slice.readUnsignedByte(), 9);
    EXPECT_EQ(slice.readerIndex(), 2U);
    EXPECT_EQ(buf.readerIndex(), 3U);
}

TEST(ByteBuf, SliceOfASliceStaysWithinIt)
{
    auto buf = Unpooled::copiedBuffer(bytes(1, 2, 3, 4));
    const auto slice = buf.slice(1, 2);
    EXPECT_EQ(slice.capacity(), 2U);
    EXPECT_EQ(ByteBufUtil::hexDump(slice.slice(1, 1)), "03");
    EXPECT_THROW((void)slice.slice(1, 2), std::out_of_range);
}

TEST(ByteBuf, CopyOwnsItsBytes)
{
    auto buf = Unpooled::copiedBuffer(bytes(1, 2, 3, 4));
    (void)buf.readByte();
    auto copy = buf.copy();
    buf.setByte(1, 9);
    EXPECT_EQ(ByteBufUtil::hexDump(copy), "020304");
    EXPECT_EQ(copy.readerIndex(), 0U);
}

TEST(ByteBuf, DuplicateKeepsTheReaderIndex)
{
    auto buf = Unpooled::copiedBuffer(bytes(1, 2, 3, 4));
    (void)buf.readShort();
    auto duplicate = buf.duplicate();
    EXPECT_EQ(duplicate.readerIndex(), 2U);
    EXPECT_EQ(duplicate.readUnsignedShort(), 0x0304);
}

TEST(ByteBuf, DiscardReadBytesShiftsTheRestAndTheMarks)
{
    auto buf = Unpooled::copiedBuffer(bytes(1, 2, 3, 4));
    (void)buf.readByte();
    buf.markReaderIndex();
    (void)buf.readByte();
    buf.discardReadBytes();
    EXPECT_EQ(buf.readerIndex(), 0U);
    EXPECT_EQ(buf.writerIndex(), 2U);
    EXPECT_EQ(ByteBufUtil::hexDump(buf), "0304");
    buf.resetReaderIndex();
    EXPECT_EQ(buf.readerIndex(), 0U);
}

TEST(ByteBuf, MovingKeepsTheBytesWhereTheyAre)
{
    auto buf = Unpooled::copiedBuffer(bytes(1, 2, 3, 4));
    const auto *data = buf.nioBuffer().data();
    auto moved = std::move(buf);
    EXPECT_EQ(moved.nioBuffer().data(), data);
    EXPECT_EQ(moved.readableBytes(), 4U);
}

TEST(ByteBuf, EqualityComparesTheReadableBytes)
{
    auto buf = Unpooled::copiedBuffer(bytes(9, 1, 2, 3));
    (void)buf.readByte();
    const auto array = bytes(1, 2, 3, 4);
    const auto other = Unpooled::copiedBuffer(std::span{array}.first(3));
    EXPECT_EQ(buf, other);
    EXPECT_EQ(buf.slice(), Unpooled::wrappedBuffer(other.nioBuffer()));
    EXPECT_NE(buf, Unpooled::copiedBuffer(bytes(1, 2, 3, 4)));
}

TEST(ByteBufView, WrapsAnArrayWithoutCopying)
{
    const auto array = bytes(1, 2, 3, 4);
    auto view = Unpooled::wrappedBuffer(array);
    EXPECT_EQ(view.nioBuffer().data(), array.data());
    EXPECT_EQ(view.readUnsignedIntLE(), 0x04030201U);
    EXPECT_THROW((void)view.readByte(), std::out_of_range);
}

TEST(ByteBufView, WriteBytesAdvancesTheSource)
{
    const auto array = bytes(1, 2, 3, 4);
    auto view = Unpooled::wrappedBuffer(array);
    auto buf = Unpooled::buffer();
    buf.writeBytes(view);
    EXPECT_FALSE(view.isReadable());
    buf.writeBytes(Unpooled::wrappedBuffer(array), 1, 2);
    EXPECT_EQ(ByteBufUtil::hexDump(buf), "010203040203");
}

TEST(ByteBuf, UnsignedVarIntsUseSevenBitGroups)
{
    auto buf = Unpooled::buffer();
    buf.writeUnsignedVarInt(0);
    buf.writeUnsignedVarInt(127);
    buf.writeUnsignedVarInt(128);
    buf.writeUnsignedVarInt(300);
    buf.writeUnsignedVarInt(0xFFFFFFFF);
    EXPECT_EQ(ByteBufUtil::hexDump(buf), "00"
                                         "7f"
                                         "8001"
                                         "ac02"
                                         "ffffffff0f");
    EXPECT_EQ(buf.readUnsignedVarInt(), 0U);
    EXPECT_EQ(buf.readUnsignedVarInt(), 127U);
    EXPECT_EQ(buf.readUnsignedVarInt(), 128U);
    EXPECT_EQ(buf.readUnsignedVarInt(), 300U);
    EXPECT_EQ(buf.readUnsignedVarInt(), 0xFFFFFFFFU);
    EXPECT_FALSE(buf.isReadable());
}

TEST(ByteBuf, SignedVarIntsAreZigzag)
{
    auto buf = Unpooled::buffer();
    buf.writeVarInt(0);
    buf.writeVarInt(-1);
    buf.writeVarInt(1);
    buf.writeVarInt(-2);
    EXPECT_EQ(ByteBufUtil::hexDump(buf), "00010203");
    for (const std::int32_t expected : {0, -1, 1, -2}) {
        EXPECT_EQ(buf.readVarInt(), expected);
    }

    for (const std::int32_t value :
         {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()}) {
        auto round = Unpooled::buffer();
        round.writeVarInt(value);
        EXPECT_EQ(round.readVarInt(), value);
    }
}

TEST(ByteBuf, VarInt64RoundTripsTheFullRange)
{
    for (const std::uint64_t value :
         {std::uint64_t{0}, std::uint64_t{1} << 35, std::numeric_limits<std::uint64_t>::max()}) {
        auto buf = Unpooled::buffer();
        buf.writeUnsignedVarInt64(value);
        EXPECT_EQ(buf.readUnsignedVarInt64(), value);
        EXPECT_FALSE(buf.isReadable());
    }
    for (const std::int64_t value :
         {std::int64_t{-1}, std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        auto buf = Unpooled::buffer();
        buf.writeVarInt64(value);
        EXPECT_EQ(buf.readVarInt64(), value);
    }
    auto buf = Unpooled::buffer();
    buf.writeUnsignedVarInt64(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(buf.readableBytes(), 10U);
}

TEST(ByteBuf, RejectsOverLongVarInts)
{
    auto six = Unpooled::buffer();
    six.writeBytes(std::array<std::byte, 6>{std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80},
                                            std::byte{0x80}, std::byte{0x00}});
    EXPECT_THROW((void)six.readUnsignedVarInt(), std::out_of_range);

    auto overflow = Unpooled::buffer();
    overflow.writeBytes(
        std::array<std::byte, 5>{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x1F}});
    EXPECT_THROW((void)overflow.readUnsignedVarInt(), std::out_of_range);

    auto eleven = Unpooled::buffer();
    eleven.writeZero(10);
    eleven.setIndex(0, 10);
    for (std::size_t i = 0; i < 10; ++i) {
        eleven.setByte(i, 0x80);
    }
    EXPECT_THROW((void)eleven.readUnsignedVarInt64(), std::out_of_range);

    auto truncated = Unpooled::buffer();
    truncated.writeByte(0x80);
    EXPECT_THROW((void)truncated.readUnsignedVarInt(), std::out_of_range);
}

TEST(ByteBuf, StringsCarryAnUnsignedVarIntLength)
{
    auto buf = Unpooled::buffer();
    buf.writeString("");
    buf.writeString("endgate");
    EXPECT_EQ(ByteBufUtil::hexDump(buf), "00"
                                         "07656e6467617465");
    EXPECT_EQ(buf.readString(16), "");
    EXPECT_EQ(buf.readString(7), "endgate");
}

TEST(ByteBuf, StringsAreBoundedByTheCapAndTheBytesPresent)
{
    auto capped = Unpooled::buffer();
    capped.writeString("endgate");
    EXPECT_THROW((void)capped.readString(6), std::out_of_range);

    auto short_body = Unpooled::buffer();
    short_body.writeUnsignedVarInt(10);
    short_body.writeByte('a');
    EXPECT_THROW((void)short_body.readString(16), std::out_of_range);

    auto huge = Unpooled::buffer();
    huge.writeUnsignedVarInt(0xFFFFFFFF);
    EXPECT_THROW((void)huge.readString(1U << 20), std::out_of_range);
}

TEST(ByteBufView, ReadsVarIntsAndStrings)
{
    auto buf = Unpooled::buffer();
    buf.writeVarInt(-300);
    buf.writeString("hi");
    auto view = buf.slice();
    EXPECT_EQ(view.readVarInt(), -300);
    EXPECT_EQ(view.readString(2), "hi");
    EXPECT_EQ(buf.readerIndex(), 0U);
}

}  // namespace
}  // namespace endgate
