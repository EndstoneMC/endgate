#include "endgate/playerlist/playerlist.h"

#include <utility>

#include "endgate/connection/connection.h"

namespace endgate {

void PlayerList::clearAll()
{
    visitProtocolVersion(connection_.getProtocolVersion().getProtocol(), [&]<int Version>() {
        using Packet = PacketOf<Version, bp::MinecraftPacketIds::PlayerList>;
        Packet packet;
        for (const auto &uuid : entries_) {
            packet.entries.emplace_back(
                typename Packet::RemoveEntry{.action = bp::PlayerListPacketType::Remove, .uuid = uuid});
        }
        connection_.delayedWrite(std::move(packet));
        return true;
    });
    clearAllSilent();
}

void PlayerList::clearAllSilent()
{
    entries_.clear();
}

}  // namespace endgate
