#include "endgate/connection/world.h"

#include <tuple>
#include <utility>

#include "endgate/connection/connection.h"

namespace endgate {

void PlayerListTracker::clear(MinecraftConnection &client)
{
    if (entries_.empty()) {
        return;
    }
    visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
        using Packet = PacketOf<Version, bp::MinecraftPacketIds::PlayerList>;
        Packet packet;
        for (const auto &uuid : entries_) {
            packet.entries.emplace_back(
                typename Packet::RemoveEntry{.action = bp::PlayerListPacketType::Remove, .uuid = uuid});
        }
        client.delayedWrite(std::move(packet));
        return true;
    });
    entries_.clear();
}

void ActorTracker::clear(MinecraftConnection &client)
{
    for (const auto actor : actors_) {
        visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
            client.delayedWrite(PacketOf<Version, bp::MinecraftPacketIds::RemoveActor>{.entity_id = actor});
            return true;
        });
    }
    actors_.clear();
    players_.clear();
}

void BossBarTracker::clear(MinecraftConnection &client)
{
    for (const auto boss : bars_) {
        visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
            PacketOf<Version, bp::MinecraftPacketIds::BossEvent> packet;
            packet.boss_id = boss;
            packet.event_type = bp::BossEventUpdateType::Remove;
            client.delayedWrite(std::move(packet));
            return true;
        });
    }
    bars_.clear();
}

void WorldState::clear(MinecraftConnection &client)
{
    std::apply([&](auto &...trackers) { (trackers.clear(client), ...); }, trackers_);
    client.flush();
}

}  // namespace endgate
