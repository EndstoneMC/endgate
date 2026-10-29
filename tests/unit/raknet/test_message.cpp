#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/network/raknet/message.h"
#include "support.h"

namespace endgate::raknet {
namespace {

using test::fromHex;
using test::toHex;

constexpr std::uint64_t GUID = 0x0123456789ABCDEF;
constexpr std::string_view MAGIC = "00ffff00fefefefefdfdfdfd12345678";

template <typename Message>
std::string encode(const Message &message)
{
    std::array<std::byte, MAXIMUM_MTU_SIZE> buffer{};
    Writer writer{buffer};
    message.encode(writer);
    EXPECT_FALSE(writer.overflowed());
    return toHex(writer.written());
}

SystemAddress endpoint(std::string_view host, std::uint16_t port)
{
    return {asio::ip::make_address(host), port};
}

std::vector<std::byte> bytes(std::string_view hex)
{
    return fromHex(hex);
}

TEST(Message, UnconnectedPingMatchesLegacyPortAndWirePy)
{
    constexpr std::string_view golden = "01010203040506070800ffff00fefefefefdfdfdfd123456780123456789abcdef";
    EXPECT_EQ(encode(UnconnectedPing{MessageId::UnconnectedPing, 0x0102030405060708, GUID}), golden);
    const auto decoded = UnconnectedPing::decode(bytes(golden));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->send_ping_time, 0x0102030405060708U);
    EXPECT_EQ(decoded->client_guid, GUID);

    const auto open = UnconnectedPing::decode(bytes("02" + std::string{golden.substr(2)}));
    ASSERT_TRUE(open);
    EXPECT_EQ(open->id, MessageId::UnconnectedPingOpenConnections);
}

TEST(Message, UnconnectedPingWithoutGuidStillDecodes)
{
    const auto decoded = UnconnectedPing::decode(bytes("010102030405060708" + std::string{MAGIC}));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->client_guid, UNASSIGNED_RAKNET_GUID);
}

TEST(Message, UnconnectedPingRejectsBadMagicAndTruncation)
{
    EXPECT_FALSE(UnconnectedPing::decode(bytes("010102030405060708"
                                               "00000000000000000000000000000000")));
    EXPECT_FALSE(UnconnectedPing::decode(bytes("010102030405060708"
                                               "00ffff00fefefefefdfdfd")));
    EXPECT_FALSE(UnconnectedPing::decode(bytes("01")));
    EXPECT_FALSE(UnconnectedPing::decode({}));
}

TEST(Message, UnconnectedPongMatchesLegacyPort)
{
    const auto data = bytes("4d4350453b");
    constexpr std::string_view golden = "1c01020304050607080123456789abcdef00ffff00fefefefefdfdfdfd123456784d4350453b";
    EXPECT_EQ(encode(UnconnectedPong{0x0102030405060708, GUID, data}), golden);
    const auto message = bytes(golden);
    const auto decoded = UnconnectedPong::decode(message);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(toHex(decoded->data), "4d4350453b");
}

TEST(Message, OpenConnectionRequest1PadsToTheProbedMtu)
{
    const auto encoded = bytes(encode(OpenConnectionRequest1{.protocol = RAKNET_PROTOCOL_VERSION, .mtu = 1200}));
    EXPECT_EQ(encoded.size(), 0x494U);
    EXPECT_EQ(toHex(std::span{encoded}.first(18)), "05" + std::string{MAGIC} + "0b");
    EXPECT_TRUE(std::ranges::all_of(std::span{encoded}.subspan(18), [](std::byte b) { return b == std::byte{0}; }));

    const auto decoded = OpenConnectionRequest1::decode(encoded);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->protocol, RAKNET_PROTOCOL_VERSION);
    EXPECT_EQ(decoded->mtu, 1200);
}

TEST(Message, OpenConnectionRequest1NeedsTheOfflineLength)
{
    EXPECT_FALSE(OpenConnectionRequest1::decode(bytes("05" + std::string{MAGIC} + "0b")));
    const auto shortest = OpenConnectionRequest1::decode(bytes("05" + std::string{MAGIC} + "0b00000000000000"));
    ASSERT_TRUE(shortest);
    EXPECT_EQ(shortest->mtu, 25 + UDP_HEADER_SIZE);
    EXPECT_FALSE(OpenConnectionRequest1::decode(bytes("05"
                                                      "00ffff00fefefefefdfdfdfd12345679"
                                                      "0b00000000000000")));
}

TEST(Message, OpenConnectionReply1MatchesLegacyPortAndWirePy)
{
    constexpr std::string_view golden = "0600ffff00fefefefefdfdfdfd123456780123456789abcdef000578";
    EXPECT_EQ(encode(OpenConnectionReply1{GUID, std::nullopt, 1400}), golden);
    const auto decoded = OpenConnectionReply1::decode(bytes(golden));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->server_guid, GUID);
    EXPECT_FALSE(decoded->cookie);
    EXPECT_EQ(decoded->mtu, 1400);
}

TEST(Message, OpenConnectionReply1WithCookieMatchesLegacyPort)
{
    constexpr std::string_view golden = "0600ffff00fefefefefdfdfdfd123456780123456789abcdef01cafebabe0578";
    EXPECT_EQ(encode(OpenConnectionReply1{GUID, 0xCAFEBABE, 1400}), golden);
    const auto decoded = OpenConnectionReply1::decode(bytes(golden));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->cookie, 0xCAFEBABEU);
    EXPECT_EQ(decoded->mtu, 1400);
    EXPECT_FALSE(OpenConnectionReply1::decode(bytes(golden.substr(0, golden.size() - 4))));
}

TEST(Message, OpenConnectionRequest2MatchesLegacyPortAndWirePy)
{
    constexpr std::string_view golden = "0700ffff00fefefefefdfdfdfd123456780480fffffe4abc05780123456789abcdef";
    EXPECT_EQ(encode(OpenConnectionRequest2{std::nullopt, endpoint("127.0.0.1", 19132), 1400, GUID}), golden);
    const auto decoded = OpenConnectionRequest2::decode(bytes(golden));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->server_address, endpoint("127.0.0.1", 19132));
    EXPECT_EQ(decoded->mtu, 1400);
    EXPECT_EQ(decoded->client_guid, GUID);
}

TEST(Message, OpenConnectionRequest2CarriesTheCookieTheWayTheReferenceClientWritesIt)
{
    EXPECT_EQ(encode(OpenConnectionRequest2{0xCAFEBABE, endpoint("127.0.0.1", 19132), 1400, GUID}),
              "0700ffff00fefefefefdfdfdfd12345678cafebabe000480fffffe4abc05780123456789abcdef");
}

TEST(Message, OpenConnectionRequest2WithoutGuidKeepsTheUnassignedGuid)
{
    const auto decoded = OpenConnectionRequest2::decode(bytes("0700ffff00fefefefefdfdfdfd123456780480fffffe4abc04b0"));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->mtu, 1200);
    EXPECT_EQ(decoded->client_guid, UNASSIGNED_RAKNET_GUID);
    EXPECT_FALSE(OpenConnectionRequest2::decode(bytes("0700ffff00fefefefefdfdfdfd123456780480fffffe4abc04")));
}

TEST(Message, OpenConnectionReply2MatchesLegacyPortAndWirePy)
{
    constexpr std::string_view golden = "0800ffff00fefefefefdfdfdfd123456780123456789abcdef043f57fef5d431057800";
    EXPECT_EQ(encode(OpenConnectionReply2{GUID, endpoint("192.168.1.10", 54321), 1400, false}), golden);
    const auto decoded = OpenConnectionReply2::decode(bytes(golden));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->client_address, endpoint("192.168.1.10", 54321));
    EXPECT_EQ(decoded->mtu, 1400);
    EXPECT_FALSE(decoded->security);
}

TEST(Message, OpenConnectionReply2SecurityIsOneBitMatchingLegacyPort)
{
    constexpr std::string_view golden = "0800ffff00fefefefefdfdfdfd123456780123456789abcdef043f57fef5d431057880";
    EXPECT_EQ(encode(OpenConnectionReply2{GUID, endpoint("192.168.1.10", 54321), 1400, true}), golden);
    EXPECT_TRUE(OpenConnectionReply2::decode(bytes(golden))->security);
}

TEST(Message, IncompatibleProtocolVersionMatchesLegacyPort)
{
    constexpr std::string_view golden = "190b00ffff00fefefefefdfdfdfd123456780123456789abcdef";
    EXPECT_EQ(encode(IncompatibleProtocolVersion{11, GUID}), golden);
    const auto decoded = IncompatibleProtocolVersion::decode(bytes(golden));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->protocol, 11);
    EXPECT_EQ(decoded->server_guid, GUID);
    EXPECT_FALSE(IncompatibleProtocolVersion::decode(bytes(std::string{golden} + "00")));
}

TEST(Message, OfflineRejectionsMatchLegacyPort)
{
    const std::array<std::pair<MessageId, std::string_view>, 4> rejections{{
        {MessageId::AlreadyConnected, "12"},
        {MessageId::NoFreeIncomingConnections, "14"},
        {MessageId::ConnectionBanned, "17"},
        {MessageId::IpRecentlyConnected, "1a"},
    }};
    for (const auto &[id, prefix] : rejections) {
        const auto golden = std::string{prefix} + std::string{MAGIC} + "0123456789abcdef";
        EXPECT_EQ(encode(OfflineRejection{id, GUID}), golden);
        const auto decoded = OfflineRejection::decode(bytes(golden));
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->id, id);
        EXPECT_EQ(decoded->server_guid, GUID);
    }
    EXPECT_FALSE(OfflineRejection::decode(bytes("13" + std::string{MAGIC} + "0123456789abcdef")));
}

TEST(Message, ConnectionRequestMatchesLegacyPortAndWirePy)
{
    constexpr std::string_view golden = "090123456789abcdef1122334455667788007077";
    const auto password = bytes("7077");
    EXPECT_EQ(encode(ConnectionRequest{GUID, 0x1122334455667788, false, password}), golden);
    const auto message = bytes(golden);
    const auto decoded = ConnectionRequest::decode(message);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->client_guid, GUID);
    EXPECT_EQ(decoded->send_ping_time, 0x1122334455667788U);
    EXPECT_EQ(toHex(decoded->password), "7077");
}

TEST(Message, ConnectionRequestWithoutSecurityByteHasNoPassword)
{
    const auto message = bytes("090123456789abcdef1122334455667788");
    const auto decoded = ConnectionRequest::decode(message);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->password.empty());
    EXPECT_FALSE(ConnectionRequest::decode(bytes("090123456789abcdef11223344556677")));
}

TEST(Message, ConnectionRequestAcceptedMatchesLegacyPortAndWirePy)
{
    std::string golden = "1004f5fffffdc3500000";
    for (std::size_t i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; ++i) {
        golden += "04ffffffff0000";
    }
    golden += "0a0b0c0d0e0f10111213141516171819";
    const ConnectionRequestAccepted accepted{.client_address = endpoint("10.0.0.2", 50000),
                                             .system_index = 0,
                                             .internal_addresses = {},
                                             .send_ping_time = 0x0A0B0C0D0E0F1011,
                                             .send_pong_time = 0x1213141516171819};
    EXPECT_EQ(encode(accepted), golden);
    const auto decoded = ConnectionRequestAccepted::decode(bytes(golden));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->client_address, endpoint("10.0.0.2", 50000));
    EXPECT_EQ(decoded->send_ping_time, 0x0A0B0C0D0E0F1011U);
    EXPECT_EQ(decoded->send_pong_time, 0x1213141516171819U);
}

TEST(Message, NewIncomingConnectionMatchesLegacyPortAndWirePy)
{
    std::string golden = "1304f5fffffe4abc";
    for (std::size_t i = 0; i < MAXIMUM_NUMBER_OF_INTERNAL_IDS; ++i) {
        golden += "04ffffffff0000";
    }
    golden += "0a0b0c0d0e0f10111213141516171819";
    const NewIncomingConnection incoming{.server_address = endpoint("10.0.0.1", 19132),
                                         .internal_addresses = {},
                                         .send_ping_time = 0x0A0B0C0D0E0F1011,
                                         .send_pong_time = 0x1213141516171819};
    EXPECT_EQ(encode(incoming), golden);
    const auto decoded = NewIncomingConnection::decode(bytes(golden));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->server_address, endpoint("10.0.0.1", 19132));
    EXPECT_EQ(decoded->send_pong_time, 0x1213141516171819U);
}

TEST(Message, ShortHandshakeMessagesKeepDefaultsLikeAFailedBitStreamRead)
{
    EXPECT_FALSE(NewIncomingConnection::decode(bytes("1304f5fffffe4abc"
                                                     "0000000000000000"
                                                     "00000000000000")));
    const auto incoming = NewIncomingConnection::decode(bytes("1304f5fffffe4abc"
                                                              "0000000000000000"
                                                              "0000000000000000"));
    ASSERT_TRUE(incoming);
    EXPECT_EQ(incoming->server_address, endpoint("10.0.0.1", 19132));
    EXPECT_EQ(incoming->send_pong_time, 0U);

    EXPECT_FALSE(ConnectionRequestAccepted::decode(bytes("1004f5fffffdc350"
                                                         "0000"
                                                         "000000000000000000000000000000")));
    EXPECT_TRUE(ConnectionRequestAccepted::decode(bytes("1004f5fffffdc350"
                                                        "0000"
                                                        "00000000000000000000000000000000")));
}

TEST(Message, ConnectedPingAndPongMatchLegacyPortAndWirePy)
{
    EXPECT_EQ(encode(ConnectedPing{0x0102030405060708}), "000102030405060708");
    EXPECT_EQ(encode(ConnectedPong{0x0102030405060708, 0x1112131415161718}), "0301020304050607081112131415161718");
    EXPECT_EQ(ConnectedPing::decode(bytes("000102030405060708"))->send_ping_time, 0x0102030405060708U);
    EXPECT_EQ(ConnectedPong::decode(bytes("0301020304050607081112131415161718"))->send_pong_time, 0x1112131415161718U);
}

TEST(Message, ConnectedPingAndPongMustBeExactLength)
{
    EXPECT_FALSE(ConnectedPing::decode(bytes("00010203040506070800")));
    EXPECT_FALSE(ConnectedPing::decode(bytes("0001020304050607")));
    EXPECT_FALSE(ConnectedPong::decode(bytes("030102030405060708111213141516171800")));
    EXPECT_FALSE(ConnectedPong::decode(bytes("03010203040506070811121314151617")));
}

TEST(Message, InvalidPasswordCarriesTheGuid)
{
    EXPECT_EQ(encode(InvalidPassword{GUID}), "180123456789abcdef");
}

TEST(Message, OfflineDecodersRefuseDatagramsTooShortToClassify)
{
    for (const auto *const id : {"05", "06", "07", "08", "12", "19"}) {
        EXPECT_FALSE(OpenConnectionRequest1::decode(bytes(id)));
        EXPECT_FALSE(OpenConnectionReply1::decode(bytes(id)));
        EXPECT_FALSE(OpenConnectionRequest2::decode(bytes(id)));
        EXPECT_FALSE(OpenConnectionReply2::decode(bytes(std::string{id} + "00")));
        EXPECT_FALSE(IncompatibleProtocolVersion::decode(bytes(id)));
        EXPECT_FALSE(OfflineRejection::decode(bytes(std::string{id} + "00")));
    }
}

TEST(Message, OfflineClassificationFollowsTheReference)
{
    EXPECT_TRUE(isOfflineMessage({}));
    EXPECT_TRUE(isOfflineMessage(bytes("8400")));
    EXPECT_TRUE(isOfflineMessage(bytes("010102030405060708" + std::string{MAGIC})));
    EXPECT_FALSE(isOfflineMessage(bytes("0101020304050607" + std::string{MAGIC})));
    EXPECT_TRUE(isOfflineMessage(bytes("05" + std::string{MAGIC} + "0b00000000000000")));
    EXPECT_FALSE(isOfflineMessage(bytes("05" + std::string{MAGIC} + "0b")));
    EXPECT_TRUE(isOfflineMessage(bytes("190b" + std::string{MAGIC} + "0123456789abcdef")));
    EXPECT_FALSE(isOfflineMessage(bytes("190b" + std::string{MAGIC} + "0123456789abcdef00")));
    EXPECT_TRUE(isOfflineMessage(bytes("1c01020304050607080123456789abcdef" + std::string{MAGIC})));
    EXPECT_FALSE(isOfflineMessage(bytes("1c01020304050607080123456789abcdef00ffff00fefefefe")));
    EXPECT_FALSE(isOfflineMessage(bytes("09" + std::string{MAGIC} + "0123456789abcdef")));
    EXPECT_FALSE(isOfflineMessage(bytes("84000000000018fe0102")));
}

}  // namespace
}  // namespace endgate::raknet
