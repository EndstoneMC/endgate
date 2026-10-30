#include <cstdint>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <bedrock/protocol/serializer.hpp>
#include <bedrock/protocol/stream.hpp>
#include <gtest/gtest.h>

#include "endgate/connection/world.h"
#include "support.h"

namespace endgate {
namespace {

constexpr bp::UUID STEVE{.most_significant_bits = 1, .least_significant_bits = 2};
constexpr bp::UUID ALEX{.most_significant_bits = 3, .least_significant_bits = 4};

template <typename Packet>
Packet decode(const ByteBuf &batch)
{
    auto view = batch.slice();
    std::ignore = view.readUnsignedVarInt();
    const auto body = view.nioBuffer();
    bp::BinaryReader reader{std::string_view{reinterpret_cast<const char *>(body.data()), body.size()}};
    return bp::deserialize<Packet>(reader).value();
}

using Added = std::pair<bp::UUID, std::int64_t>;

template <int Version>
PacketOf<Version, bp::MinecraftPacketIds::PlayerList> playerList(std::initializer_list<Added> added,
                                                                 std::initializer_list<bp::UUID> removed = {})
{
    using Packet = PacketOf<Version, bp::MinecraftPacketIds::PlayerList>;
    Packet packet;
    for (const auto &[uuid, id] : added) {
        packet.entries.emplace_back(typename Packet::AddEntry{
            .action = bp::PlayerListPacketType::Add, .uuid = uuid, .id = bp::ActorUniqueID{id}});
    }
    for (const auto &uuid : removed) {
        packet.entries.emplace_back(
            typename Packet::RemoveEntry{.action = bp::PlayerListPacketType::Remove, .uuid = uuid});
    }
    return packet;
}

std::vector<std::uint32_t> idsOf(const std::vector<ByteBuf> &batches)
{
    std::vector<std::uint32_t> ids;
    for (const auto &batch : batches) {
        auto view = batch.slice();
        ids.push_back(view.readUnsignedVarInt());
    }
    return ids;
}

class WorldStateTest : public testing::Test {
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

    ProxyServer server_;
    Registry registry_;
    Node node_{server_, registry_};
};

TEST_F(WorldStateTest, ThePlayerListIsClearedEntryByEntry)
{
    PlayerListTracker tracker;
    tracker.track(playerList<MAXIMUM_VERSION.getProtocol()>({{STEVE, 70}, {ALEX, 80}}));
    tracker.track(playerList<MAXIMUM_VERSION.getProtocol()>({}, {ALEX}));

    const auto connection = client();
    node_.call([&] {
        tracker.clear(*connection);
        return 0;
    });

    ASSERT_EQ(connection->batches().size(), 1U);
    const auto entries = decode<bp::PlayerListPacket>(connection->batches()[0]).entries;
    ASSERT_EQ(entries.size(), 1U);
    const auto &removed = std::get<bp::PlayerListPacket::RemoveEntry>(entries[0]);
    EXPECT_EQ(removed.action, bp::PlayerListPacketType::Remove);
    EXPECT_EQ(removed.uuid.most_significant_bits, STEVE.most_significant_bits);
    EXPECT_EQ(removed.uuid.least_significant_bits, STEVE.least_significant_bits);
}

TEST_F(WorldStateTest, AnEmptyPlayerListSendsNothing)
{
    PlayerListTracker tracker;
    tracker.track(playerList<MAXIMUM_VERSION.getProtocol()>({{STEVE, 70}}, {STEVE}));

    const auto connection = client();
    node_.call([&] {
        tracker.clear(*connection);
        return 0;
    });

    EXPECT_EQ(connection->batches().size(), 0U);
}

TEST_F(WorldStateTest, ThePlayerListIsClearedAtTheClientsOwnVersion)
{
    constexpr auto protocol = MINIMUM_VERSION.getProtocol();
    PlayerListTracker tracker;
    tracker.track(playerList<protocol>({{STEVE, 70}}));

    const auto connection = client(MINIMUM_VERSION);
    node_.call([&] {
        tracker.clear(*connection);
        return 0;
    });

    ASSERT_EQ(connection->batches().size(), 1U);
    EXPECT_EQ(decode<bp::PlayerListPacket_<protocol>>(connection->batches()[0]).entries.size(), 1U);
}

TEST_F(WorldStateTest, EveryBossBarStillUpIsRemoved)
{
    BossBarTracker tracker;
    for (const auto boss : {7, 8, 9}) {
        bp::BossEventPacket packet;
        packet.boss_id = bp::ActorUniqueID{boss};
        packet.event_type = bp::BossEventUpdateType::Add;
        tracker.track(packet);
    }
    bp::BossEventPacket gone;
    gone.boss_id = bp::ActorUniqueID{8};
    gone.event_type = bp::BossEventUpdateType::Remove;
    tracker.track(gone);

    const auto connection = client();
    node_.call([&] {
        tracker.clear(*connection);
        return 0;
    });

    ASSERT_EQ(connection->batches().size(), 2U);
    std::vector<std::int64_t> removed;
    for (const auto &batch : connection->batches()) {
        const auto packet = decode<bp::BossEventPacket>(batch);
        EXPECT_EQ(packet.event_type, bp::BossEventUpdateType::Remove);
        removed.push_back(packet.boss_id);
    }
    EXPECT_EQ(removed, (std::vector<std::int64_t>{7, 9}));
}

TEST_F(WorldStateTest, AnUpdateLeavesTheBarUp)
{
    BossBarTracker tracker;
    bp::BossEventPacket added;
    added.boss_id = bp::ActorUniqueID{7};
    added.event_type = bp::BossEventUpdateType::Add;
    tracker.track(added);
    bp::BossEventPacket renamed;
    renamed.boss_id = bp::ActorUniqueID{7};
    renamed.event_type = bp::BossEventUpdateType::UpdateName;
    tracker.track(renamed);

    const auto connection = client();
    node_.call([&] {
        tracker.clear(*connection);
        return 0;
    });

    EXPECT_EQ(connection->batches().size(), 1U);
}

TEST_F(WorldStateTest, ActorsAreTakenBackOneByOne)
{
    ActorTracker tracker;
    tracker.track(bp::AddActorPacket{.entity_id = bp::ActorUniqueID{7}});
    tracker.track(bp::AddPaintingPacket{.entity_id = bp::ActorUniqueID{8}});
    tracker.track(bp::AddItemActorPacket{.id = bp::ActorUniqueID{9}});
    tracker.track(bp::RemoveActorPacket{.entity_id = bp::ActorUniqueID{8}});

    const auto connection = client();
    node_.call([&] {
        tracker.clear(*connection);
        return 0;
    });

    ASSERT_EQ(connection->batches().size(), 2U);
    std::vector<std::int64_t> removed;
    for (const auto &batch : connection->batches()) {
        removed.push_back(decode<bp::RemoveActorPacket>(batch).entity_id);
    }
    EXPECT_EQ(removed, (std::vector<std::int64_t>{7, 9}));
}

TEST_F(WorldStateTest, APlayersActorComesFromItsPlayerListEntry)
{
    ActorTracker tracker;
    tracker.track(playerList<MAXIMUM_VERSION.getProtocol()>({{STEVE, 70}, {ALEX, 80}}));
    tracker.track(playerList<MAXIMUM_VERSION.getProtocol()>({}, {ALEX}));

    const auto connection = client();
    node_.call([&] {
        tracker.clear(*connection);
        return 0;
    });

    ASSERT_EQ(connection->batches().size(), 1U);
    EXPECT_EQ(decode<bp::RemoveActorPacket>(connection->batches()[0]).entity_id, 70);
}

TEST_F(WorldStateTest, EachPacketReachesEveryTrackerThatTakesIt)
{
    WorldState state;
    state.track(playerList<MAXIMUM_VERSION.getProtocol()>({{STEVE, 70}}));
    bp::BossEventPacket boss;
    boss.boss_id = bp::ActorUniqueID{7};
    boss.event_type = bp::BossEventUpdateType::Add;
    state.track(boss);
    state.track(bp::DisconnectPacket{});

    const auto connection = client();
    node_.call([&] {
        state.clear(*connection);
        return 0;
    });

    EXPECT_EQ(
        idsOf(connection->batches()),
        (std::vector<std::uint32_t>{bp::RemoveActorPacket::Id, bp::PlayerListPacket::Id, bp::BossEventPacket::Id}));
}

TEST_F(WorldStateTest, ClearingForgetsWhatItSent)
{
    WorldState state;
    state.track(playerList<MAXIMUM_VERSION.getProtocol()>({{STEVE, 70}}));

    const auto connection = client();
    node_.call([&] {
        state.clear(*connection);
        state.clear(*connection);
        return 0;
    });

    EXPECT_EQ(connection->batches().size(), 2U);
}

}  // namespace
}  // namespace endgate
