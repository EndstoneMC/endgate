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
        bp::PlayerListPacket_<Version> packet;
        for (const auto &uuid : entries_) {
            packet.entries.emplace_back(typename bp::PlayerListPacket_<Version>::RemoveEntry{
                .action = bp::PlayerListPacketType::Remove, .uuid = uuid});
        }
        client.delayedWrite(std::move(packet));
        return true;
    });
    entries_.clear();
}

void BossBarTracker::clear(MinecraftConnection &client)
{
    for (const auto boss : bars_) {
        visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
            bp::BossEventPacket_<Version> packet;
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
