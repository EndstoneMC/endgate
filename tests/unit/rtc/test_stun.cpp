#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/rtc/stun.h"

namespace endgate::rtc {
namespace {

std::vector<std::byte> bytesOf(std::string_view raw)
{
    const auto bytes = std::as_bytes(std::span{raw.data(), raw.size()});
    return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> vectorOf(const ByteBuf &buf)
{
    const auto bytes = buf.nioBuffer();
    return {bytes.begin(), bytes.end()};
}

const std::vector<std::byte> SAMPLE_REQUEST =
    bytesOf(std::string_view{"\x00\x01\x00\x58"
                             "\x21\x12\xa4\x42"
                             "\xb7\xe7\xa7\x01\xbc\x34\xd6\x86\xfa\x87\xdf\xae"
                             "\x80\x22\x00\x10"
                             "STUN test client"
                             "\x00\x24\x00\x04"
                             "\x6e\x00\x01\xff"
                             "\x80\x29\x00\x08"
                             "\x93\x2f\xf9\xb1\x51\x26\x3b\x36"
                             "\x00\x06\x00\x09"
                             "\x65\x76\x74\x6a\x3a\x68\x36\x76\x59\x20\x20\x20"
                             "\x00\x08\x00\x14"
                             "\x9a\xea\xa7\x0c\xbf\xd8\xcb\x56\x78\x1e\xf2\xb5"
                             "\xb2\xd3\xf2\x49\xc1\xb5\x71\xa2"
                             "\x80\x28\x00\x04"
                             "\xe5\x7a\x3b\xcf",
                             108});

const std::vector<std::byte> SAMPLE_IPV4_RESPONSE =
    bytesOf(std::string_view{"\x01\x01\x00\x3c"
                             "\x21\x12\xa4\x42"
                             "\xb7\xe7\xa7\x01\xbc\x34\xd6\x86\xfa\x87\xdf\xae"
                             "\x80\x22\x00\x0b"
                             "\x74\x65\x73\x74\x20\x76\x65\x63\x74\x6f\x72\x20"
                             "\x00\x20\x00\x08"
                             "\x00\x01\xa1\x47\xe1\x12\xa6\x43"
                             "\x00\x08\x00\x14"
                             "\x2b\x91\xf5\x99\xfd\x9e\x90\xc3\x8c\x74\x89\xf9"
                             "\x2a\xf9\xba\x53\xf0\x6b\xe7\xd7"
                             "\x80\x28\x00\x04"
                             "\xc0\x7d\x4c\x96",
                             80});

const std::vector<std::byte> SAMPLE_IPV6_RESPONSE =
    bytesOf(std::string_view{"\x01\x01\x00\x48"
                             "\x21\x12\xa4\x42"
                             "\xb7\xe7\xa7\x01\xbc\x34\xd6\x86\xfa\x87\xdf\xae"
                             "\x80\x22\x00\x0b"
                             "\x74\x65\x73\x74\x20\x76\x65\x63\x74\x6f\x72\x20"
                             "\x00\x20\x00\x14"
                             "\x00\x02\xa1\x47"
                             "\x01\x13\xa9\xfa\xa5\xd3\xf1\x79"
                             "\xbc\x25\xf4\xb5\xbe\xd2\xb9\xd9"
                             "\x00\x08\x00\x14"
                             "\xa3\x82\x95\x4e\x4b\xe6\x7b\xf1\x17\x84\xc9\x7c"
                             "\x82\x92\xc2\x75\xbf\xe3\xed\x41"
                             "\x80\x28\x00\x04"
                             "\xc8\xfb\x0b\x4c",
                             92});

const std::vector<std::byte> SAMPLE_LONG_TERM_REQUEST =
    bytesOf(std::string_view{"\x00\x01\x00\x60"
                             "\x21\x12\xa4\x42"
                             "\x78\xad\x34\x33\xc6\xad\x72\xc0\x29\xda\x41\x2e"
                             "\x00\x06\x00\x12"
                             "\xe3\x83\x9e\xe3\x83\x88\xe3\x83\xaa\xe3\x83\x83"
                             "\xe3\x82\xaf\xe3\x82\xb9\x00\x00"
                             "\x00\x15\x00\x1c"
                             "\x66\x2f\x2f\x34\x39\x39\x6b\x39\x35\x34\x64\x36"
                             "\x4f\x4c\x33\x34\x6f\x4c\x39\x46\x53\x54\x76\x79"
                             "\x36\x34\x73\x41"
                             "\x00\x14\x00\x0b"
                             "\x65\x78\x61\x6d\x70\x6c\x65\x2e\x6f\x72\x67\x00"
                             "\x00\x08\x00\x14"
                             "\xf6\x70\x24\x65\x6d\xd6\x4a\x3e\x02\xb8\xe0\x71"
                             "\x2e\x85\xc9\xa2\x8c\xa8\x96\x66",
                             116});

constexpr std::string_view SAMPLE_PASSWORD = "VOkJxbRl1RmTxUk/WvJxBt";

TEST(StunMessageTest, DecodesTheRfc5769Request)
{
    const auto message = StunMessage::decode(Unpooled::wrappedBuffer(SAMPLE_REQUEST));
    ASSERT_TRUE(message);
    EXPECT_EQ(message->message_class, StunClass::Request);
    EXPECT_EQ(message->method, StunMethod::Binding);
    EXPECT_EQ(message->username, "evtj:h6vY");
    EXPECT_EQ(message->software, "STUN test client");
    EXPECT_EQ(message->priority, 0x6e0001ffU);
    EXPECT_EQ(message->ice_controlled, 0x932ff9b151263b36ULL);
    EXPECT_FALSE(message->ice_controlling);
    EXPECT_FALSE(message->use_candidate);
    EXPECT_TRUE(message->fingerprint);
    EXPECT_TRUE(message->unknown_comprehension_required.empty());
    EXPECT_TRUE(message->verifyIntegrity(Unpooled::wrappedBuffer(SAMPLE_REQUEST), SAMPLE_PASSWORD));
    EXPECT_FALSE(message->verifyIntegrity(Unpooled::wrappedBuffer(SAMPLE_REQUEST), "wrong password"));
}

TEST(StunMessageTest, DecodesFromTheReaderIndex)
{
    auto buf = Unpooled::buffer();
    buf.writeByte(0xFF);
    buf.writeBytes(SAMPLE_REQUEST);
    auto datagram = buf.slice();
    datagram.skipBytes(1);
    const auto message = StunMessage::decode(datagram);
    ASSERT_TRUE(message);
    EXPECT_EQ(message->username, "evtj:h6vY");
    EXPECT_TRUE(message->verifyIntegrity(datagram, SAMPLE_PASSWORD));
    EXPECT_EQ(StunMessage::peekLocalUfrag(datagram), "evtj");
}

TEST(StunMessageTest, DecodesTheRfc5769Ipv4Response)
{
    const auto message = StunMessage::decode(Unpooled::wrappedBuffer(SAMPLE_IPV4_RESPONSE));
    ASSERT_TRUE(message);
    EXPECT_EQ(message->message_class, StunClass::SuccessResponse);
    EXPECT_EQ(message->software, "test vector");
    EXPECT_EQ(message->xor_mapped_address, asio::ip::udp::endpoint(asio::ip::make_address("192.0.2.1"), 32853));
    EXPECT_TRUE(message->verifyIntegrity(Unpooled::wrappedBuffer(SAMPLE_IPV4_RESPONSE), SAMPLE_PASSWORD));
}

TEST(StunMessageTest, DecodesTheRfc5769Ipv6Response)
{
    const auto message = StunMessage::decode(Unpooled::wrappedBuffer(SAMPLE_IPV6_RESPONSE));
    ASSERT_TRUE(message);
    EXPECT_EQ(message->xor_mapped_address,
              asio::ip::udp::endpoint(asio::ip::make_address("2001:db8:1234:5678:11:2233:4455:6677"), 32853));
    EXPECT_TRUE(message->verifyIntegrity(Unpooled::wrappedBuffer(SAMPLE_IPV6_RESPONSE), SAMPLE_PASSWORD));
}

TEST(StunMessageTest, ReportsTheLongTermAttributesItDoesNotUnderstand)
{
    const auto message = StunMessage::decode(Unpooled::wrappedBuffer(SAMPLE_LONG_TERM_REQUEST));
    ASSERT_TRUE(message);
    EXPECT_FALSE(message->fingerprint);
    EXPECT_TRUE(message->integrity_offset);
    EXPECT_EQ(message->unknown_comprehension_required, (std::vector<std::uint16_t>{0x0015, 0x0014}));
}

TEST(StunMessageTest, PeeksTheLocalUfrag)
{
    EXPECT_EQ(StunMessage::peekLocalUfrag(Unpooled::wrappedBuffer(SAMPLE_REQUEST)), "evtj");
    EXPECT_FALSE(StunMessage::peekLocalUfrag(Unpooled::wrappedBuffer(SAMPLE_IPV4_RESPONSE)));
}

TEST(StunMessageTest, EncodesWhatItDecodes)
{
    StunMessage request;
    request.transaction_id = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},  std::byte{5},  std::byte{6},
                              std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12}};
    request.username = "local:remote";
    request.priority = 1853824767;
    request.use_candidate = true;
    request.ice_controlling = 0x0102030405060708ULL;
    const auto datagram = request.encode("password");

    const auto decoded = StunMessage::decode(datagram.slice());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->transaction_id, request.transaction_id);
    EXPECT_EQ(decoded->username, "local:remote");
    EXPECT_EQ(decoded->priority, 1853824767U);
    EXPECT_TRUE(decoded->use_candidate);
    EXPECT_EQ(decoded->ice_controlling, 0x0102030405060708ULL);
    EXPECT_TRUE(decoded->fingerprint);
    EXPECT_TRUE(decoded->verifyIntegrity(datagram.slice(), "password"));
    EXPECT_EQ(StunMessage::peekLocalUfrag(datagram.slice()), "local");
}

TEST(StunMessageTest, EncodesAnErrorResponseWithItsUnknownAttributes)
{
    StunMessage response;
    response.message_class = StunClass::ErrorResponse;
    response.error_code = StunErrorCode{.code = 420, .reason = "Unknown Attribute"};
    response.unknown_attributes = {0x0014, 0x0015, 0x0016};
    const auto datagram = response.encode("password");

    const auto decoded = StunMessage::decode(datagram.slice());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->message_class, StunClass::ErrorResponse);
    ASSERT_TRUE(decoded->error_code);
    EXPECT_EQ(decoded->error_code->code, 420);
    EXPECT_EQ(decoded->error_code->reason, "Unknown Attribute");
    EXPECT_EQ(decoded->unknown_attributes, response.unknown_attributes);
    EXPECT_TRUE(decoded->verifyIntegrity(datagram.slice(), "password"));
}

TEST(StunMessageTest, EncodesBothAddressFamilies)
{
    for (const auto *address : {"203.0.113.7", "2001:db8::1"}) {
        StunMessage response;
        response.message_class = StunClass::SuccessResponse;
        response.xor_mapped_address = asio::ip::udp::endpoint(asio::ip::make_address(address), 54321);
        const auto decoded = StunMessage::decode(response.encode(std::nullopt).slice());
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->xor_mapped_address, response.xor_mapped_address);
    }
}

TEST(StunMessageTest, IgnoresComprehensionOptionalAttributes)
{
    StunMessage request;
    request.username = "local:remote";
    auto datagram = vectorOf(request.encode(std::nullopt));
    datagram.resize(datagram.size() - 8);
    const std::array<std::byte, 8> goog_network_info{std::byte{0xC0}, std::byte{0x57}, std::byte{0x00},
                                                     std::byte{0x04}, std::byte{0x00}, std::byte{0x01},
                                                     std::byte{0x00}, std::byte{0x0A}};
    datagram.insert(datagram.begin() + 20, goog_network_info.begin(), goog_network_info.end());
    datagram[3] = static_cast<std::byte>(datagram.size() - 20);
    const auto message = StunMessage::decode(Unpooled::wrappedBuffer(datagram));
    ASSERT_TRUE(message);
    EXPECT_TRUE(message->unknown_comprehension_required.empty());
    EXPECT_EQ(message->username, "local:remote");
}

TEST(StunMessageTest, RejectsAHeaderThatIsNotStun)
{
    auto datagram = SAMPLE_REQUEST;
    datagram[0] = std::byte{0x40};
    EXPECT_FALSE(StunMessage::decode(Unpooled::wrappedBuffer(datagram)));

    datagram = SAMPLE_REQUEST;
    datagram[4] = std::byte{0x00};
    EXPECT_FALSE(StunMessage::decode(Unpooled::wrappedBuffer(datagram)));

    EXPECT_FALSE(StunMessage::decode(Unpooled::wrappedBuffer(std::span{SAMPLE_REQUEST}.first(19))));
    EXPECT_FALSE(StunMessage::decode(ByteBufView{}));
}

TEST(StunMessageTest, RejectsALengthThatDisagreesWithTheDatagram)
{
    auto datagram = SAMPLE_REQUEST;
    datagram.push_back(std::byte{0});
    EXPECT_FALSE(StunMessage::decode(Unpooled::wrappedBuffer(datagram)));

    datagram = SAMPLE_REQUEST;
    datagram[3] = std::byte{0x5A};
    EXPECT_FALSE(StunMessage::decode(Unpooled::wrappedBuffer(datagram)));

    datagram = SAMPLE_REQUEST;
    datagram.resize(datagram.size() - 4);
    EXPECT_FALSE(StunMessage::decode(Unpooled::wrappedBuffer(datagram)));
}

TEST(StunMessageTest, RejectsAnAttributeThatOverrunsTheMessage)
{
    auto datagram = SAMPLE_REQUEST;
    datagram[22] = std::byte{0xFF};
    EXPECT_FALSE(StunMessage::decode(Unpooled::wrappedBuffer(datagram)));
    EXPECT_FALSE(StunMessage::peekLocalUfrag(Unpooled::wrappedBuffer(datagram)));
}

TEST(StunMessageTest, RejectsAWrongFingerprint)
{
    auto datagram = SAMPLE_REQUEST;
    datagram.back() ^= std::byte{0x01};
    EXPECT_FALSE(StunMessage::decode(Unpooled::wrappedBuffer(datagram)));
}

TEST(StunMessageTest, RejectsAnAttributeAfterTheFingerprint)
{
    StunMessage request;
    auto datagram = vectorOf(request.encode(std::nullopt));
    const std::array<std::byte, 4> use_candidate{std::byte{0x00}, std::byte{0x25}, std::byte{0x00}, std::byte{0x00}};
    datagram.insert(datagram.end(), use_candidate.begin(), use_candidate.end());
    datagram[3] = static_cast<std::byte>(datagram.size() - 20);
    EXPECT_FALSE(StunMessage::decode(Unpooled::wrappedBuffer(datagram)));
}

TEST(StunMessageTest, RejectsFixedSizeAttributesOfTheWrongSize)
{
    auto datagram = SAMPLE_REQUEST;
    datagram[43] = std::byte{0x03};
    EXPECT_FALSE(StunMessage::decode(Unpooled::wrappedBuffer(datagram)));
}

TEST(StunMessageTest, IgnoresAttributesAfterMessageIntegrity)
{
    StunMessage request;
    request.username = "a:b";
    auto datagram = vectorOf(request.encode("key"));
    const std::array<std::byte, 8> priority{std::byte{0x00}, std::byte{0x24}, std::byte{0x00}, std::byte{0x04},
                                            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
    datagram.resize(datagram.size() - 8);
    datagram.insert(datagram.end(), priority.begin(), priority.end());
    const auto message = StunMessage::decode(Unpooled::wrappedBuffer(datagram));
    ASSERT_TRUE(message);
    EXPECT_FALSE(message->fingerprint);
    EXPECT_FALSE(message->priority);
    EXPECT_EQ(message->username, "a:b");
}

TEST(StunMessageTest, PeekRejectsAUsernameWithoutAColon)
{
    StunMessage request;
    request.username = "nocolon";
    EXPECT_FALSE(StunMessage::peekLocalUfrag(request.encode("key").slice()));
    request.username = ":remote";
    EXPECT_FALSE(StunMessage::peekLocalUfrag(request.encode("key").slice()));
}

TEST(StunMessageTest, SurvivesEveryTruncationAndBitFlip)
{
    for (std::size_t length = 0; length <= SAMPLE_REQUEST.size(); ++length) {
        const auto prefix = std::span{SAMPLE_REQUEST}.first(length);
        (void)StunMessage::decode(Unpooled::wrappedBuffer(prefix));
        (void)StunMessage::peekLocalUfrag(Unpooled::wrappedBuffer(prefix));
    }
    for (std::size_t index = 0; index < SAMPLE_REQUEST.size(); ++index) {
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            auto datagram = SAMPLE_REQUEST;
            datagram[index] ^= static_cast<std::byte>(1U << bit);
            (void)StunMessage::decode(Unpooled::wrappedBuffer(datagram));
            (void)StunMessage::peekLocalUfrag(Unpooled::wrappedBuffer(datagram));
        }
    }
}

}  // namespace
}  // namespace endgate::rtc
