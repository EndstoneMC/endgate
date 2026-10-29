#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include <gtest/gtest.h>

#include "endgate/network/raknet/codec.h"
#include "support.h"

namespace endgate::raknet {
namespace {

using test::fromHex;
using test::toHex;

std::string encodeAddress(const SystemAddress &address)
{
    auto buffer = Unpooled::buffer(64, 64);
    writeAddress(buffer, address);
    return toHex(buffer);
}

SystemAddress endpoint(std::string_view host, std::uint16_t port)
{
    return {asio::ip::make_address(host), port};
}

TEST(Codec, IntegersAreBigEndianExceptTheTwentyFourBitCounters)
{
    auto buffer = Unpooled::buffer(32, 32);
    buffer.writeByte(0x01);
    buffer.writeShort(0x0203);
    buffer.writeMediumLE(0x123456);
    buffer.writeInt(0x04050607);
    buffer.writeLong(0x08090A0B0C0D0E0F);
    buffer.writeFloat(1.5F);
    EXPECT_EQ(toHex(buffer), "010203563412040506070809"
                             "0a0b0c0d0e0f3fc00000");

    EXPECT_EQ(buffer.readUnsignedByte(), 0x01);
    EXPECT_EQ(buffer.readUnsignedShort(), 0x0203);
    EXPECT_EQ(buffer.readUnsignedMediumLE(), 0x123456U);
    EXPECT_EQ(buffer.readUnsignedInt(), 0x04050607U);
    EXPECT_EQ(buffer.readLong(), 0x08090A0B0C0D0E0F);
    EXPECT_EQ(buffer.readFloat(), 1.5F);
    EXPECT_FALSE(buffer.isReadable());
}

TEST(Codec, TwentyFourBitCounterMatchesRakNet)
{
    auto buffer = Unpooled::buffer(3, 3);
    buffer.writeMediumLE(0x123456);
    EXPECT_EQ(toHex(buffer), "563412");
}

TEST(Codec, Ipv4AddressMatchesRakNetAndWirePy)
{
    EXPECT_EQ(encodeAddress(endpoint("127.0.0.1", 19132)), "0480fffffe4abc");
    EXPECT_EQ(encodeAddress(endpoint("192.168.1.10", 54321)), "043f57fef5d431");
    EXPECT_EQ(encodeAddress(SystemAddress{}), "04ffffffff0000");
}

TEST(Codec, Ipv6AddressMatchesRakNet)
{
    EXPECT_EQ(encodeAddress(endpoint("fe80::1234:5678", 19132)),
              "060a004abc00000000fe80000000000000000000001234567800000000");
    EXPECT_EQ(encodeAddress(endpoint("2001:db8::1", 443)),
              "060a0001bb0000000020010db800000000000000000000000100000000");
}

TEST(Codec, AddressesRoundTrip)
{
    const std::array addresses{endpoint("127.0.0.1", 19132), endpoint("255.255.255.255", 65535), endpoint("0.0.0.0", 0),
                               endpoint("::1", 1),
                               SystemAddress{asio::ip::address_v6{asio::ip::address_v6::bytes_type{0xFE, 0x80}, 7}, 9}};
    for (const auto &address : addresses) {
        const auto bytes = fromHex(encodeAddress(address));
        ByteBufView reader = bytes;
        EXPECT_TRUE(isAddressReadable(reader));
        EXPECT_EQ(readAddress(reader), address);
        EXPECT_FALSE(reader.isReadable());
    }
}

TEST(Codec, Ipv6FamilyIsIgnoredOnRead)
{
    const auto windows = fromHex("0617004abc00000000fe80000000000000000000001234567800000000");
    ByteBufView reader = windows;
    EXPECT_EQ(readAddress(reader), endpoint("fe80::1234:5678", 19132));
}

TEST(Codec, AnyVersionOtherThanFourIsReadAsIpv6)
{
    const auto bytes = fromHex("00"
                               "0a004abc00000000fe80000000000000000000001234567800000000");
    ByteBufView reader = bytes;
    EXPECT_EQ(readAddress(reader), endpoint("fe80::1234:5678", 19132));
}

TEST(Codec, TruncatedReadsThrowWithoutMoving)
{
    const auto bytes = fromHex("0102");
    ByteBufView reader = bytes;
    EXPECT_THROW(static_cast<void>(reader.readUnsignedMediumLE()), std::out_of_range);
    EXPECT_EQ(reader.readerIndex(), 0U);
    EXPECT_THROW(static_cast<void>(reader.readUnsignedInt()), std::out_of_range);
    EXPECT_THROW(static_cast<void>(reader.readLong()), std::out_of_range);
    EXPECT_THROW(static_cast<void>(reader.readSlice(3)), std::out_of_range);
    EXPECT_EQ(reader.readUnsignedShort(), 0x0102);
    EXPECT_THROW(static_cast<void>(reader.readUnsignedByte()), std::out_of_range);
}

TEST(Codec, TruncatedAddressesAreNotReadable)
{
    for (const auto *hex : {"", "04", "0480ffff", "0480fffffe4a", "06", "060a004abc00000000fe80",
                            "0617004abc00000000fe8000000000000000000000123456780000"}) {
        const auto bytes = fromHex(hex);
        ByteBufView reader = bytes;
        EXPECT_FALSE(isAddressReadable(reader)) << hex;
        EXPECT_THROW(static_cast<void>(readAddress(reader)), std::out_of_range) << hex;
    }
}

}  // namespace
}  // namespace endgate::raknet
