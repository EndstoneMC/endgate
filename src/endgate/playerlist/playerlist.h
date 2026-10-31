#pragma once

#include <set>
#include <utility>
#include <variant>

#include <bedrock/protocol/uuid.hpp>

#include "endgate/protocol/state.h"

namespace endgate {

class MinecraftConnection;

/**
 * @see com.velocitypowered.api.proxy.player.TabList
 * @see com.velocitypowered.proxy.tablist.VelocityTabListLegacy
 */
class PlayerList {
public:
    explicit PlayerList(MinecraftConnection &connection) : connection_(connection) {}

    /**
     * Clears all entries from the player list.
     *
     * @see com.velocitypowered.api.proxy.player.TabList#clearAll
     */
    void clearAll();

    /**
     * @see com.velocitypowered.proxy.tablist.InternalTabList#clearAllSilent
     */
    void clearAllSilent();

    /**
     * @see com.velocitypowered.proxy.tablist.InternalTabList#processLegacy
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::PlayerList>
    void processLegacy(const Packet &packet)
    {
        for (const auto &entry : packet.entries) {
            if (const auto *const added = std::get_if<typename Packet::AddEntry>(&entry)) {
                entries_.insert(added->uuid);
                continue;
            }
            entries_.erase(std::get<typename Packet::RemoveEntry>(entry).uuid);
        }
    }

private:
    struct UuidLess {
        bool operator()(const bp::UUID &left, const bp::UUID &right) const
        {
            return std::pair{left.most_significant_bits, left.least_significant_bits} <
                   std::pair{right.most_significant_bits, right.least_significant_bits};
        }
    };

    MinecraftConnection &connection_;
    std::set<bp::UUID, UuidLess> entries_;
};

}  // namespace endgate
