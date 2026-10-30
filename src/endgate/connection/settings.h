#pragma once

#include <cstdint>
#include <optional>

#include <bedrock/protocol/chunk.h>
#include <bedrock/protocol/network.h>

#include "endgate/protocol/state.h"

namespace endgate {

class MinecraftConnection;

/**
 * What the client told its first backend about itself. A later backend hears none of it, since the
 * client says it once a session, so the proxy says it again on the client's behalf.
 *
 * @see com.velocitypowered.proxy.connection.client.ClientPlaySessionHandler#handleBackendJoinGame
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

    /**
     * Tells a backend what the client already told the one before it. Call on the player's event loop.
     */
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
