#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <bedrock/protocol/serializer.hpp>
#include <bedrock/protocol/stream.hpp>
#include <gtest/gtest.h>

#include "endgate/backend.h"
#include "endgate/connection/backend.h"
#include "endgate/connection/player.h"
#include "support.h"

namespace endgate {
namespace {

constexpr int PROTOCOL = MAXIMUM_VERSION.getProtocol();
constexpr bp::UUID STEVE{.most_significant_bits = 1, .least_significant_bits = 2};

template <bp::MinecraftPacketIds Id>
using Latest = PacketOf<PROTOCOL, Id>;

template <typename Packet>
Packet decode(const ByteBuf &batch)
{
    auto view = batch.slice();
    std::ignore = view.readUnsignedVarInt();
    const auto body = view.nioBuffer();
    bp::BinaryReader reader{std::string_view{reinterpret_cast<const char *>(body.data()), body.size()}};
    return bp::deserialize<Packet>(reader).value();
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

Latest<bp::MinecraftPacketIds::PlayerList> listed(bp::ActorUniqueID id)
{
    Latest<bp::MinecraftPacketIds::PlayerList> packet;
    packet.entries.emplace_back(Latest<bp::MinecraftPacketIds::PlayerList>::AddEntry{
        .action = bp::PlayerListPacketType::Add, .uuid = STEVE, .id = id});
    return packet;
}

Latest<bp::MinecraftPacketIds::StartGame> startGame(std::uint64_t runtime_id, std::int64_t unique_id)
{
    Latest<bp::MinecraftPacketIds::StartGame> packet;
    packet.runtime_id = bp::ActorRuntimeID{runtime_id};
    packet.entity_id = bp::ActorUniqueID{unique_id};
    return packet;
}

Latest<bp::MinecraftPacketIds::PlayerList> delisted()
{
    Latest<bp::MinecraftPacketIds::PlayerList> packet;
    packet.entries.emplace_back(Latest<bp::MinecraftPacketIds::PlayerList>::RemoveEntry{
        .action = bp::PlayerListPacketType::Remove, .uuid = STEVE});
    return packet;
}

class PlayTest : public testing::Test {
protected:
    void SetUp() override
    {
        client_ = std::make_shared<TestConnection>(node_.endpoint(), "127.0.0.1:1", Direction::Serverbound);
        player_ = std::make_shared<Player>(server_, client_);
        client_handler_ = std::make_shared<ClientPlaySessionHandler>(player_);
        server_connection_ = std::make_shared<ServerConnection>(backend_server_, player_);
        node_.call([&] {
            client_->setProtocolVersion(MAXIMUM_VERSION);
            client_->setSessionHandler(client_handler_);
            backend_handler_ = std::make_shared<BackendPlaySessionHandler>(server_connection_);
            return 0;
        });
    }

    template <typename Packet>
    void fromBackend(const Packet &packet)
    {
        node_.call([&] { return backend_handler_->handle(packet); });
    }

    [[nodiscard]] std::shared_ptr<TestConnection> backend()
    {
        auto connection = std::make_shared<TestConnection>(node_.endpoint(), "127.0.0.1:2", Direction::Clientbound);
        node_.call([&] {
            connection->setProtocolVersion(MAXIMUM_VERSION);
            return 0;
        });
        return connection;
    }

    ProxyServer server_;
    Registry registry_;
    Node node_{server_, registry_};
    BackendServer backend_server_{ServerInfo::parse("lobby", "127.0.0.1:19132").value()};
    std::shared_ptr<TestConnection> client_;
    std::shared_ptr<Player> player_;
    std::shared_ptr<ClientPlaySessionHandler> client_handler_;
    std::shared_ptr<ServerConnection> server_connection_;
    std::shared_ptr<BackendPlaySessionHandler> backend_handler_;
};

TEST_F(PlayTest, TheActorsABackendSpawnedAreKeptPlayerSide)
{
    Latest<bp::MinecraftPacketIds::AddPlayer> player;
    player.abilities_data.target_player = bp::ActorUniqueID{6};

    fromBackend(Latest<bp::MinecraftPacketIds::AddActor>{.entity_id = bp::ActorUniqueID{5}});
    fromBackend(player);
    fromBackend(Latest<bp::MinecraftPacketIds::AddItemActor>{.id = bp::ActorUniqueID{7}});
    fromBackend(Latest<bp::MinecraftPacketIds::AddPainting>{.entity_id = bp::ActorUniqueID{8}});
    fromBackend(Latest<bp::MinecraftPacketIds::RemoveActor>{.entity_id = bp::ActorUniqueID{7}});

    EXPECT_EQ(client_handler_->getServerActors(),
              (std::set{bp::ActorUniqueID{5}, bp::ActorUniqueID{6}, bp::ActorUniqueID{8}}));
}

TEST_F(PlayTest, APlayerTakenOffThePlayerListKeepsItsActor)
{
    Latest<bp::MinecraftPacketIds::AddPlayer> npc;
    npc.abilities_data.target_player = bp::ActorUniqueID{6};

    fromBackend(listed(bp::ActorUniqueID{6}));
    fromBackend(npc);
    fromBackend(delisted());

    EXPECT_EQ(client_handler_->getServerActors(), (std::set{bp::ActorUniqueID{6}}));
}

TEST_F(PlayTest, APlayerListEntryIsNoActor)
{
    fromBackend(listed(bp::ActorUniqueID{10}));

    EXPECT_TRUE(client_handler_->getServerActors().empty());
}

TEST_F(PlayTest, BossBarsFollowAddAndRemove)
{
    Latest<bp::MinecraftPacketIds::BossEvent> add;
    add.event_type = bp::BossEventUpdateType::Add;
    add.boss_id = bp::ActorUniqueID{9};
    fromBackend(add);
    add.boss_id = bp::ActorUniqueID{10};
    fromBackend(add);
    Latest<bp::MinecraftPacketIds::BossEvent> remove;
    remove.event_type = bp::BossEventUpdateType::Remove;
    remove.boss_id = bp::ActorUniqueID{9};
    fromBackend(remove);

    EXPECT_EQ(client_handler_->getServerBossBars(), (std::vector{bp::ActorUniqueID{10}}));
}

TEST_F(PlayTest, ObjectivesFollowDisplayAndRemove)
{
    fromBackend(Latest<bp::MinecraftPacketIds::SetDisplayObjective>{.objective_name = "kills"});
    fromBackend(Latest<bp::MinecraftPacketIds::SetDisplayObjective>{.objective_name = "deaths"});
    fromBackend(Latest<bp::MinecraftPacketIds::RemoveObjective>{.objective_name = "kills"});

    EXPECT_EQ(client_handler_->getServerObjectives(), (std::set<std::string>{"deaths"}));
}

TEST_F(PlayTest, OnlyThePlayersOwnEffectsAreKept)
{
    using MobEffect = Latest<bp::MinecraftPacketIds::MobEffect>;
    node_.call([&] {
        client_handler_->handleBackendStartGame(startGame(1, 10), *server_connection_);
        return 0;
    });

    MobEffect effect;
    effect.event_id = MobEffect::Event::Add;
    effect.runtime_id = bp::ActorRuntimeID{1};
    effect.effect_id = 3;
    fromBackend(effect);
    effect.effect_id = 4;
    fromBackend(effect);
    effect.runtime_id = bp::ActorRuntimeID{2};
    effect.effect_id = 5;
    fromBackend(effect);
    effect.event_id = MobEffect::Event::Remove;
    effect.runtime_id = bp::ActorRuntimeID{1};
    effect.effect_id = 4;
    fromBackend(effect);

    EXPECT_EQ(client_handler_->getServerEffects(), (std::set<std::int32_t>{3}));
}

TEST_F(PlayTest, TheOpenContainerIsKeptUntilItCloses)
{
    Latest<bp::MinecraftPacketIds::ContainerOpen> open;
    open.container_id = static_cast<decltype(open.container_id)>(2);
    fromBackend(open);
    ASSERT_TRUE(client_handler_->getServerContainer().has_value());
    EXPECT_EQ(client_handler_->getServerContainer()->first, 2);

    fromBackend(Latest<bp::MinecraftPacketIds::ContainerClose>{});

    EXPECT_FALSE(client_handler_->getServerContainer().has_value());
}

TEST_F(PlayTest, DebugShapesFollowDrawAndRemove)
{
    Latest<bp::MinecraftPacketIds::PrimitiveShapesPacket> drawn;
    for (const std::uint64_t id : {1U, 2U, 3U}) {
        auto &shape = drawn.shapes.emplace_back();
        shape.network_id = id;
        shape.shape_type.emplace();
    }
    fromBackend(drawn);
    Latest<bp::MinecraftPacketIds::PrimitiveShapesPacket> removed;
    removed.shapes.emplace_back().network_id = 2;
    fromBackend(removed);

    EXPECT_EQ(client_handler_->getServerShapes(), (std::set<std::uint64_t>{1, 3}));
}

TEST_F(PlayTest, ChunksAreKeptHoweverFarThePlayerMoves)
{
    using LevelChunk = Latest<bp::MinecraftPacketIds::FullChunkData>;
    fromBackend(LevelChunk{.pos = {.x = 0, .z = 0}});
    fromBackend(LevelChunk{.pos = {.x = -3, .z = 2}});
    fromBackend(LevelChunk{.pos = {.x = 4000, .z = 0}});

    EXPECT_EQ(client_handler_->getServerChunks(),
              (std::set<std::pair<std::int32_t, std::int32_t>>{{-3, 2}, {0, 0}, {4000, 0}}));
}

TEST_F(PlayTest, ADimensionChangeLeavesNoChunks)
{
    fromBackend(Latest<bp::MinecraftPacketIds::FullChunkData>{.pos = {.x = 1, .z = 1}});
    Latest<bp::MinecraftPacketIds::ChangeDimension> change;
    change.dimension_id = static_cast<decltype(change.dimension_id)>(1);
    fromBackend(change);

    EXPECT_TRUE(client_handler_->getServerChunks().empty());
    EXPECT_EQ(client_handler_->getDimension(), 1);
}

TEST_F(PlayTest, TheFirstBackendsRegistriesReachTheClient)
{
    EXPECT_FALSE(
        node_.call([&] { return backend_handler_->handle(Latest<bp::MinecraftPacketIds::ItemRegistryPacket>{}); }));
}

TEST_F(PlayTest, TheFirstStartGameReachesTheClientWithoutClientSideGeneration)
{
    auto start_game = startGame(1, 10);
    start_game.server_enabled_client_side_generation = true;
    start_game.settings.seed = 4604325614334842756;

    node_.call([&] {
        client_handler_->handleBackendStartGame(start_game, *server_connection_);
        client_->flush();
        return 0;
    });

    ASSERT_EQ(idsOf(client_->batches()), (std::vector<std::uint32_t>{Latest<bp::MinecraftPacketIds::StartGame>::Id}));
    const auto sent = decode<Latest<bp::MinecraftPacketIds::StartGame>>(client_->batches()[0]);
    EXPECT_FALSE(sent.server_enabled_client_side_generation);
    EXPECT_EQ(sent.settings.seed, 0);
}

TEST_F(PlayTest, ALaterStartGameNeedsTheBackendLeg)
{
    node_.call([&] {
        client_handler_->handleBackendStartGame(startGame(1, 10), *server_connection_);
        return 0;
    });

    EXPECT_TRUE(node_.call([&] {
        try {
            client_handler_->handleBackendStartGame(startGame(2, 20), *server_connection_);
        }
        catch (const std::logic_error &) {
            return true;
        }
        return false;
    }));
}

TEST_F(PlayTest, AKickFromTheConnectedServerNamesItAndItsReason)
{
    node_.call([&] {
        player_->setConnectedServer(server_connection_);
        return 0;
    });
    Latest<bp::MinecraftPacketIds::Disconnect> kick;
    kick.reason = decltype(kick.reason)::BadPacket;
    kick.messages = std::monostate{};

    fromBackend(kick);

    ASSERT_EQ(idsOf(client_->batches()), (std::vector<std::uint32_t>{Latest<bp::MinecraftPacketIds::Disconnect>::Id}));
    const auto disconnect = decode<Latest<bp::MinecraftPacketIds::Disconnect>>(client_->batches()[0]);
    const auto *const messages = std::get_if<bp::DisconnectPacketMessages>(&disconnect.messages);
    ASSERT_NE(messages, nullptr);
    EXPECT_EQ(messages->message, "You were kicked from lobby: badpacket");
    EXPECT_TRUE(client_->closed());
}

TEST_F(PlayTest, TheClientsSettingsAreSaidAgainToTheNextBackend)
{
    node_.call([&] {
        client_handler_->handle(Latest<bp::MinecraftPacketIds::ClientCacheStatus>{.enabled = true});
        client_handler_->handle(
            Latest<bp::MinecraftPacketIds::RequestChunkRadius>{.chunk_radius = 12, .max_chunk_radius = 16});
        return client_handler_->handle(
            Latest<bp::MinecraftPacketIds::RequestChunkRadius>{.chunk_radius = 8, .max_chunk_radius = 16});
    });
    const auto next = backend();

    node_.call([&] {
        player_->getPlayerSettings().replay(*next);
        return 0;
    });

    ASSERT_EQ(idsOf(next->batches()),
              (std::vector<std::uint32_t>{bp::ClientCacheStatusPacket::Id, bp::RequestChunkRadiusPacket::Id}));
    EXPECT_TRUE(decode<bp::ClientCacheStatusPacket>(next->batches()[0]).enabled);
    EXPECT_EQ(decode<bp::RequestChunkRadiusPacket>(next->batches()[1]).chunk_radius, 8);
}

TEST_F(PlayTest, ASettingTheClientNeverSentIsNotMadeUp)
{
    node_.call([&] { return client_handler_->handle(Latest<bp::MinecraftPacketIds::ClientCacheStatus>{}); });
    const auto next = backend();

    node_.call([&] {
        player_->getPlayerSettings().replay(*next);
        return 0;
    });

    EXPECT_EQ(idsOf(next->batches()), (std::vector<std::uint32_t>{bp::ClientCacheStatusPacket::Id}));
}

}  // namespace
}  // namespace endgate
