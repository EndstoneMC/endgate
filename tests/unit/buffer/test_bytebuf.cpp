#include <array>
#include <cstddef>
#include <cstdint>
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
    buf.writeByte(0x01).writeShort(0x0203).writeShortLE(0x0405).writeMedium(0x060708).writeMediumLE(0x090A0B);
    buf.writeInt(0x0C0D0E0F).writeIntLE(0x10111213).writeLong(0x1415161718191A1B).writeLongLE(0x1C1D1E1F20212223);
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
    buf.writeBoolean(true).writeByte(0xFF).writeShort(0xFFFE).writeShortLE(0x1234).writeMedium(0x800000);
    buf.writeMediumLE(0x123456).writeInt(0xFFFFFFFF).writeIntLE(0x89ABCDEF).writeLong(0xFFFFFFFFFFFFFFFF);
    buf.writeLongLE(0x0123456789ABCDEF).writeFloat(1.5F).writeFloatLE(-2.25F).writeDouble(3.5).writeDoubleLE(-0.125);

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
    buf.writeShort(0xBEEF).writeInt(0x01020304);
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

}  // namespace
}  // namespace endgate
