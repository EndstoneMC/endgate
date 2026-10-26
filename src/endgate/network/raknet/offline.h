#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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
        virtual void sendTo(const SystemAddress &address, std::span<const std::byte> datagram) = 0;

    protected:
        ~Listener() = default;
    };

    OfflineHandler(Listener &listener, std::uint64_t guid) : listener_(listener), guid_(guid) {}

    [[nodiscard]] std::uint64_t getGuid() const { return guid_; }

    /**
     * @see RakNet::RakPeer::SetOfflinePingResponse
     */
    void setOfflinePingResponse(std::span<const std::byte> data)
    {
        offline_ping_response_.assign(data.begin(), data.end());
    }

    [[nodiscard]] std::optional<OpenConnectionRequest2> handle(std::span<const std::byte> datagram,
                                                               const SystemAddress &address,
                                                               bool allow_incoming_connections);

    void reject(const SystemAddress &address, MessageId reason);

private:
    Listener &listener_;
    std::uint64_t guid_;
    std::vector<std::byte> offline_ping_response_;
};

}  // namespace endgate::raknet
