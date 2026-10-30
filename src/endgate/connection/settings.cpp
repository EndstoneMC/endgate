#include "endgate/connection/settings.h"

#include "endgate/connection/connection.h"

namespace endgate {

void ClientSettings::replay(MinecraftConnection &backend) const
{
    visitProtocolVersion(backend.getProtocolVersion().getProtocol(), [&]<int Version>() {
        if (cache_enabled_) {
            backend.delayedWrite(
                PacketOf<Version, bp::MinecraftPacketIds::ClientCacheStatus>{.enabled = *cache_enabled_});
        }
        if (chunk_radius_) {
            backend.delayedWrite(PacketOf<Version, bp::MinecraftPacketIds::RequestChunkRadius>{
                .chunk_radius = chunk_radius_->radius, .max_chunk_radius = chunk_radius_->max});
        }
        return true;
    });
    backend.flush();
}

}  // namespace endgate
