#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/raknet/codec.h"
#include "endgate/network/raknet/message.h"
#include "endgate/network/raknet/protocol.h"

namespace endgate::raknet {

/**
 * @see RakNet::ProcessOfflineNetworkPacket
 */
class OfflineHandler {
public:
    class Listener {
    public:
        virtual void sendTo(const SystemAddress &address, ByteBufView datagram) = 0;

    protected:
        ~Listener() = default;
    };

    OfflineHandler(Listener &listener, std::uint64_t guid) : listener_(listener), guid_(guid) {}

    [[nodiscard]] std::uint64_t getGuid() const { return guid_; }

    /**
     * @see RakNet::RakPeer::SetOfflinePingResponse
     */
    void setOfflinePingResponse(ByteBufView data) { offline_ping_response_ = data.copy(); }

    [[nodiscard]] std::optional<OpenConnectionRequest2> handle(ByteBufView datagram, const SystemAddress &address,
                                                               bool allow_incoming_connections);

    void reject(const SystemAddress &address, MessageId reason);

private:
    Listener &listener_;
    std::uint64_t guid_;
    ByteBuf offline_ping_response_;
};

}  // namespace endgate::raknet
