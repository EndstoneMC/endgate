#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <bedrock/protocol/actor.h>
#include <bedrock/protocol/boss.h>
#include <bedrock/protocol/effect.h>
#include <bedrock/protocol/inventory.h>
#include <bedrock/protocol/player.h>
#include <bedrock/protocol/scoreboard.h>
#include <bedrock/protocol/sound.h>
#include <bedrock/protocol/text.h>
#include <bedrock/protocol/ui.h>
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
 * The objectives a backend put on the client's scoreboard. Removing an objective takes its scores
 * with it, so the names are all that is kept.
 */
class ScoreboardTracker {
public:
    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::SetDisplayObjective))
    void track(const Packet &packet)
    {
        objectives_.insert(packet.objective_name);
    }

    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::RemoveObjective))
    void track(const Packet &packet)
    {
        objectives_.erase(packet.objective_name);
    }

    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::SetScore))
    void track(const Packet &packet)
    {
        for (const auto &info : packet.score_info) {
            std::visit(
                [&](const auto &score) {
                    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(score.objective_name)>, std::string>) {
                        objectives_.insert(score.objective_name);
                    }
                },
                info);
        }
    }

    void clear(MinecraftConnection &client);

private:
    std::set<std::string> objectives_;
};

/**
 * The effects a backend put on actors the client can see. An actor the switch removes anyway only
 * costs a MobEffect the client ignores, which is cheaper than knowing whose effects these are.
 */
class EffectTracker {
public:
    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::MobEffect))
    void track(const Packet &packet)
    {
        if (packet.event_id == Packet::Event::Remove) {
            effects_.erase({packet.runtime_id, packet.effect_id});
            return;
        }
        effects_.insert({packet.runtime_id, packet.effect_id});
    }

    void clear(MinecraftConnection &client);

private:
    std::set<std::pair<bp::ActorRuntimeID, std::int32_t>> effects_;
};

/**
 * Whether a backend left a form on the client's screen. The client answers a form to the backend that
 * asked, which endgate never reads, so a form already answered costs one close the client ignores.
 */
class FormTracker {
public:
    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::ShowModalForm))
    void track(const Packet &)
    {
        open_ = true;
    }

    void clear(MinecraftConnection &client);

private:
    bool open_{false};
};

/**
 * The container a backend opened for the client. Closing it names the same container, so its id and
 * type are kept as the numbers the backend sent at the client's own version.
 */
class ContainerTracker {
public:
    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::ContainerOpen))
    void track(const Packet &packet)
    {
        open_ = {.id = static_cast<std::int32_t>(packet.container_id), .type = static_cast<std::int32_t>(packet.type)};
    }

    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::ContainerClose))
    void track(const Packet &)
    {
        open_.reset();
    }

    void clear(MinecraftConnection &client);

private:
    struct OpenContainer {
        std::int32_t id;
        std::int32_t type;
    };

    std::optional<OpenContainer> open_;
};

/**
 * Whether a backend started a sound the client may still be playing. One StopSound stops them all, so
 * nothing but that is kept.
 */
class SoundTracker {
public:
    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::PlaySound))
    void track(const Packet &)
    {
        playing_ = true;
    }

    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::StopSound))
    void track(const Packet &packet)
    {
        if (packet.stop_all) {
            playing_ = false;
        }
    }

    void clear(MinecraftConnection &client);

private:
    bool playing_{false};
};

/**
 * Whether a backend left a title on the client's screen. Times sets how long the next one shows and
 * puts none up itself.
 */
class TitleTracker {
public:
    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::SetTitle))
    void track(const Packet &packet)
    {
        if (packet.type == Packet::TitleType::Times) {
            return;
        }
        shown_ = packet.type != Packet::TitleType::Clear && packet.type != Packet::TitleType::Reset;
    }

    void clear(MinecraftConnection &client);

private:
    bool shown_{false};
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
    std::tuple<ActorTracker, PlayerListTracker, BossBarTracker, ScoreboardTracker, EffectTracker, FormTracker,
               ContainerTracker, SoundTracker, TitleTracker>
        trackers_;
};

}  // namespace endgate
