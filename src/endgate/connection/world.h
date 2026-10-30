#pragma once

#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <variant>

#include <bedrock/protocol/actor.h>
#include <bedrock/protocol/boss.h>
#include <bedrock/protocol/player.h>
#include <bedrock/protocol/uuid.hpp>

#include "endgate/protocol/state.h"

namespace endgate {

class MinecraftConnection;

struct UuidLess {
    bool operator()(const bp::UUID &left, const bp::UUID &right) const
    {
        return std::pair{left.most_significant_bits, left.least_significant_bits} <
               std::pair{right.most_significant_bits, right.least_significant_bits};
    }
};

/**
 * The entries a backend put in the client's player list.
 *
 * @see com.velocitypowered.proxy.connection.client.ClientPlaySessionHandler#handleBackendJoinGame
 */
class PlayerListTracker {
public:
    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::PlayerList))
    void track(const Packet &packet)
    {
        for (const auto &entry : packet.entries) {
            if (const auto *const added = std::get_if<typename Packet::AddEntry>(&entry)) {
                entries_.insert(added->uuid);
                continue;
            }
            entries_.erase(std::get<typename Packet::RemoveEntry>(entry).uuid);
        }
    }

    void clear(MinecraftConnection &client);

private:
    std::set<bp::UUID, UuidLess> entries_;
};

/**
 * The actors a backend spawned for the client. AddPlayer carries no unique ID, so a player's comes
 * from its player list entry; an entry for a player that never spawned only costs a RemoveActor the
 * client ignores.
 */
class ActorTracker {
public:
    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::AddActor) ||
                 Packet::Id == static_cast<int>(bp::MinecraftPacketIds::AddPainting))
    void track(const Packet &packet)
    {
        actors_.insert(packet.entity_id);
    }

    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::AddItemActor))
    void track(const Packet &packet)
    {
        actors_.insert(packet.id);
    }

    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::RemoveActor))
    void track(const Packet &packet)
    {
        actors_.erase(packet.entity_id);
    }

    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::PlayerList))
    void track(const Packet &packet)
    {
        for (const auto &entry : packet.entries) {
            if (const auto *const added = std::get_if<typename Packet::AddEntry>(&entry)) {
                players_.insert_or_assign(added->uuid, added->id);
                actors_.insert(added->id);
                continue;
            }
            const auto found = players_.find(std::get<typename Packet::RemoveEntry>(entry).uuid);
            if (found != players_.end()) {
                actors_.erase(found->second);
                players_.erase(found);
            }
        }
    }

    void clear(MinecraftConnection &client);

private:
    std::set<bp::ActorUniqueID> actors_;
    std::map<bp::UUID, bp::ActorUniqueID, UuidLess> players_;
};

/**
 * The boss bars a backend showed the client.
 *
 * @see com.velocitypowered.proxy.connection.client.ClientPlaySessionHandler#getServerBossBars
 */
class BossBarTracker {
public:
    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::BossEvent))
    void track(const Packet &packet)
    {
        if (packet.event_type == bp::BossEventUpdateType::Add) {
            bars_.insert(packet.boss_id);
        }
        else if (packet.event_type == bp::BossEventUpdateType::Remove) {
            bars_.erase(packet.boss_id);
        }
    }

    void clear(MinecraftConnection &client);

private:
    std::set<bp::ActorUniqueID> bars_;
};

/**
 * What the client holds because of one backend. Bedrock has no second StartGame to reset it, so every
 * kind of state is followed as the backend creates it and taken back before the player moves on.
 */
class WorldState {
public:
    template <typename Packet>
    void track(const Packet &packet)
    {
        std::apply(
            [&](auto &...trackers) {
                (
                    [&] {
                        if constexpr (requires { trackers.track(packet); }) {
                            trackers.track(packet);
                        }
                    }(),
                    ...);
            },
            trackers_);
    }

    /**
     * Sends what takes back everything this backend gave the client, and forgets it. Call on the
     * client's event loop.
     */
    void clear(MinecraftConnection &client);

private:
    std::tuple<ActorTracker, PlayerListTracker, BossBarTracker> trackers_;
};

}  // namespace endgate
