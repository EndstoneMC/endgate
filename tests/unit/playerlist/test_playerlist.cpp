#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include <bedrock/protocol/serializer.hpp>
#include <bedrock/protocol/stream.hpp>
#include <gtest/gtest.h>

#include "endgate/playerlist/playerlist.h"
#include "support.h"

namespace endgate {
namespace {

constexpr bp::UUID STEVE{.most_significant_bits = 1, .least_significant_bits = 2};
constexpr bp::UUID ALEX{.most_significant_bits = 3, .least_significant_bits = 4};

template <int Version>
PacketOf<Version, bp::MinecraftPacketIds::PlayerList> playerList(std::initializer_list<bp::UUID> added,
                                                                 std::initializer_list<bp::UUID> removed = {})
{
    using Packet = PacketOf<Version, bp::MinecraftPacketIds::PlayerList>;
    Packet packet;
    for (const auto &uuid : added) {
        packet.entries.emplace_back(typename Packet::AddEntry{.action = bp::PlayerListPacketType::Add, .uuid = uuid});
    }
    for (const auto &uuid : removed) {
        packet.entries.emplace_back(
            typename Packet::RemoveEntry{.action = bp::PlayerListPacketType::Remove, .uuid = uuid});
    }
    return packet;
}

template <typename Packet>
Packet decode(const ByteBuf &batch)
{
    auto view = batch.slice();
    std::ignore = view.readUnsignedVarInt();
    const auto body = view.nioBuffer();
    bp::BinaryReader reader{std::string_view{reinterpret_cast<const char *>(body.data()), body.size()}};
    return bp::deserialize<Packet>(reader).value();
}

class PlayerListTest : public testing::Test {
protected:
    [[nodiscard]] std::shared_ptr<TestConnection> client(ProtocolVersion version = MAXIMUM_VERSION)
    {
        auto connection = std::make_shared<TestConnection>(node_.endpoint(), "127.0.0.1:1", Direction::Serverbound);
        node_.call([&] {
            connection->setProtocolVersion(version);
            return 0;
        });
        return connection;
    }

    void clearAll(PlayerList &player_list, MinecraftConnection &connection)
    {
        node_.call([&] {
            player_list.clearAll();
            connection.flush();
            return 0;
        });
    }

    ProxyServer server_;
    Registry registry_;
    Node node_{server_, registry_};
};

TEST_F(PlayerListTest, ClearAllRemovesTheEntriesTheBackendLeft)
{
    const auto connection = client();
    PlayerList player_list{*connection};
    player_list.processLegacy(playerList<MAXIMUM_VERSION.getProtocol()>({STEVE, ALEX}));
    player_list.processLegacy(playerList<MAXIMUM_VERSION.getProtocol()>({}, {ALEX}));

    clearAll(player_list, *connection);

    ASSERT_EQ(connection->batches().size(), 1U);
    const auto entries = decode<bp::PlayerListPacket>(connection->batches()[0]).entries;
    ASSERT_EQ(entries.size(), 1U);
    const auto &removed = std::get<bp::PlayerListPacket::RemoveEntry>(entries[0]);
    EXPECT_EQ(removed.action, bp::PlayerListPacketType::Remove);
    EXPECT_EQ(removed.uuid.most_significant_bits, STEVE.most_significant_bits);
    EXPECT_EQ(removed.uuid.least_significant_bits, STEVE.least_significant_bits);
}

TEST_F(PlayerListTest, ClearAllForgetsWhatItRemoved)
{
    const auto connection = client();
    PlayerList player_list{*connection};
    player_list.processLegacy(playerList<MAXIMUM_VERSION.getProtocol()>({STEVE}));

    clearAll(player_list, *connection);
    clearAll(player_list, *connection);

    ASSERT_EQ(connection->batches().size(), 2U);
    EXPECT_TRUE(decode<bp::PlayerListPacket>(connection->batches()[1]).entries.empty());
}

TEST_F(PlayerListTest, ClearAllSpeaksTheClientsVersion)
{
    constexpr auto protocol = MINIMUM_VERSION.getProtocol();
    using PlayerListPacket = PacketOf<protocol, bp::MinecraftPacketIds::PlayerList>;
    const auto connection = client(MINIMUM_VERSION);
    PlayerList player_list{*connection};
    player_list.processLegacy(playerList<protocol>({STEVE}));

    clearAll(player_list, *connection);

    ASSERT_EQ(connection->batches().size(), 1U);
    EXPECT_EQ(decode<PlayerListPacket>(connection->batches()[0]).entries.size(), 1U);
}

TEST_F(PlayerListTest, ClearAllSilentSendsNothing)
{
    const auto connection = client();
    PlayerList player_list{*connection};
    player_list.processLegacy(playerList<MAXIMUM_VERSION.getProtocol()>({STEVE}));

    node_.call([&] {
        player_list.clearAllSilent();
        connection->flush();
        return 0;
    });
    clearAll(player_list, *connection);

    ASSERT_EQ(connection->batches().size(), 1U);
    EXPECT_TRUE(decode<bp::PlayerListPacket>(connection->batches()[0]).entries.empty());
}

}  // namespace
}  // namespace endgate
