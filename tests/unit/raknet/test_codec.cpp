#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "endgate/network/raknet/codec.h"
#include "support.h"

namespace endgate::raknet {
namespace {

using test::fromHex;
using test::toHex;

std::string encodeAddress(const SystemAddress &address)
{
    std::array<std::byte, 64> buffer{};
    Writer writer{buffer};
    writer.writeAddress(address);
    return toHex(writer.written());
}

SystemAddress endpoint(std::string_view host, std::uint16_t port)
{
    return {asio::ip::make_address(host), port};
}

TEST(Codec, IntegersAreBigEndianExceptTheTwentyFourBitCounters)
{
    std::array<std::byte, 32> buffer{};
    Writer writer{buffer};
    writer.writeUint8(0x01);
    writer.writeUint16(0x0203);
    writer.writeUint24(0x123456);
    writer.writeUint32(0x04050607);
    writer.writeUint64(0x08090A0B0C0D0E0F);
    writer.writeFloat(1.5F);
    EXPECT_EQ(toHex(writer.written()), "010203563412040506070809"
                                       "0a0b0c0d0e0f3fc00000");

    Reader reader{writer.written()};
    EXPECT_EQ(reader.readUint8(), 0x01);
    EXPECT_EQ(reader.readUint16(), 0x0203);
    EXPECT_EQ(reader.readUint24(), 0x123456U);
    EXPECT_EQ(reader.readUint32(), 0x04050607U);
    EXPECT_EQ(reader.readUint64(), 0x08090A0B0C0D0E0FU);
    EXPECT_EQ(reader.readFloat(), 1.5F);
    EXPECT_EQ(reader.remaining(), 0U);
}

TEST(Codec, TwentyFourBitCounterMatchesLegacyPort)
{
    std::array<std::byte, 3> buffer{};
    Writer writer{buffer};
    writer.writeUint24(0x123456);
    EXPECT_EQ(toHex(writer.written()), "563412");
}

TEST(Codec, Ipv4AddressMatchesLegacyPortAndWirePy)
{
    EXPECT_EQ(encodeAddress(endpoint("127.0.0.1", 19132)), "0480fffffe4abc");
    EXPECT_EQ(encodeAddress(endpoint("192.168.1.10", 54321)), "043f57fef5d431");
    EXPECT_EQ(encodeAddress(SystemAddress{}), "04ffffffff0000");
}

TEST(Codec, Ipv6AddressMatchesLegacyPort)
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
        Reader reader{bytes};
        EXPECT_EQ(reader.readAddress(), address);
        EXPECT_EQ(reader.remaining(), 0U);
    }
}

TEST(Codec, Ipv6FamilyIsIgnoredOnRead)
{
    const auto windows = fromHex("0617004abc00000000fe80000000000000000000001234567800000000");
    Reader reader{windows};
    EXPECT_EQ(reader.readAddress(), endpoint("fe80::1234:5678", 19132));
}

TEST(Codec, AnyVersionOtherThanFourIsReadAsIpv6)
{
    const auto bytes = fromHex("00"
                               "0a004abc00000000fe80000000000000000000001234567800000000");
    Reader reader{bytes};
    EXPECT_EQ(reader.readAddress(), endpoint("fe80::1234:5678", 19132));
}

TEST(Codec, TruncatedReadsFailWithoutMoving)
{
    const auto bytes = fromHex("0102");
    Reader reader{bytes};
    EXPECT_FALSE(reader.readUint24());
    EXPECT_EQ(reader.offset(), 0U);
    EXPECT_FALSE(reader.readUint32());
    EXPECT_FALSE(reader.readUint64());
    EXPECT_FALSE(reader.readBytes(3));
    EXPECT_EQ(reader.readUint16(), 0x0102);
    EXPECT_FALSE(reader.readUint8());
}

TEST(Codec, TruncatedAddressesFail)
{
    for (const auto *hex : {"", "04", "0480ffff", "0480fffffe4a", "06", "060a004abc00000000fe80",
                            "0617004abc00000000fe8000000000000000000000123456780000"}) {
        const auto bytes = fromHex(hex);
        Reader reader{bytes};
        EXPECT_FALSE(reader.readAddress()) << hex;
    }
}

TEST(Codec, WriterOverflowIsRecordedNotOverrun)
{
    std::array<std::byte, 4> buffer{};
    Writer writer{std::span{buffer}.first(3)};
    writer.writeUint16(0xAAAA);
    writer.writeUint16(0xBBBB);
    EXPECT_TRUE(writer.overflowed());
    EXPECT_EQ(writer.size(), 2U);
    EXPECT_EQ(buffer[2], std::byte{0});
    writer.writeUint8(0xCC);
    EXPECT_EQ(writer.size(), 2U);
}

}  // namespace
}  // namespace endgate::raknet
