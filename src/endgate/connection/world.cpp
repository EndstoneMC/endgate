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

void ScoreboardTracker::clear(MinecraftConnection &client)
{
    for (const auto &objective : objectives_) {
        visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
            client.delayedWrite(
                PacketOf<Version, bp::MinecraftPacketIds::RemoveObjective>{.objective_name = objective});
            return true;
        });
    }
    objectives_.clear();
}

void EffectTracker::clear(MinecraftConnection &client)
{
    for (const auto &[actor, effect] : effects_) {
        visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
            using Packet = PacketOf<Version, bp::MinecraftPacketIds::MobEffect>;
            Packet packet;
            packet.runtime_id = actor;
            packet.event_id = Packet::Event::Remove;
            packet.effect_id = effect;
            client.delayedWrite(std::move(packet));
            return true;
        });
    }
    effects_.clear();
}

void FormTracker::clear(MinecraftConnection &client)
{
    if (!std::exchange(open_, false)) {
        return;
    }
    visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
        client.delayedWrite(PacketOf<Version, bp::MinecraftPacketIds::ClientboundCloseScreen>{});
        return true;
    });
}

void ContainerTracker::clear(MinecraftConnection &client)
{
    const auto open = std::exchange(open_, std::nullopt);
    if (!open) {
        return;
    }
    visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
        using Packet = PacketOf<Version, bp::MinecraftPacketIds::ContainerClose>;
        Packet packet;
        packet.container_id = static_cast<decltype(packet.container_id)>(open->id);
        packet.container_type = static_cast<decltype(packet.container_type)>(open->type);
        packet.server_initiated_close = true;
        client.delayedWrite(std::move(packet));
        return true;
    });
}

void SoundTracker::clear(MinecraftConnection &client)
{
    if (!std::exchange(playing_, false)) {
        return;
    }
    visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
        client.delayedWrite(PacketOf<Version, bp::MinecraftPacketIds::StopSound>{.stop_all = true});
        return true;
    });
}

void TitleTracker::clear(MinecraftConnection &client)
{
    if (!std::exchange(shown_, false)) {
        return;
    }
    visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
        using Packet = PacketOf<Version, bp::MinecraftPacketIds::SetTitle>;
        Packet packet;
        packet.type = Packet::TitleType::Clear;
        client.delayedWrite(std::move(packet));
        return true;
    });
}

void WorldState::clear(MinecraftConnection &client)
{
    std::apply([&](auto &...trackers) { (trackers.clear(client), ...); }, trackers_);
    client.flush();
}

}  // namespace endgate
