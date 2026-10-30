#pragma once

#include <cstdint>
#include <optional>

#include <bedrock/protocol/chunk.h>
#include <bedrock/protocol/network.h>

#include "endgate/protocol/state.h"

namespace endgate {

class MinecraftConnection;

/**
 * Represents the client settings for the player.
 *
 * @see com.velocitypowered.api.proxy.player.PlayerSettings
 * @see com.velocitypowered.proxy.connection.client.ClientSettingsWrapper
 */
class ClientSettings {
public:
    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::RequestChunkRadius))
    void track(const Packet &packet)
    {
        chunk_radius_ = {.radius = packet.chunk_radius, .max = packet.max_chunk_radius};
    }

    template <typename Packet>
        requires(Packet::Id == static_cast<int>(bp::MinecraftPacketIds::ClientCacheStatus))
    void track(const Packet &packet)
    {
        cache_enabled_ = packet.enabled;
    }

    void replay(MinecraftConnection &backend) const;

private:
    struct ChunkRadius {
        std::int32_t radius;
        std::uint8_t max;
    };

    std::optional<ChunkRadius> chunk_radius_;
    std::optional<bool> cache_enabled_;
};

}  // namespace endgate
