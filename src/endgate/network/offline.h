#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "endgate/network/buffer.h"
#include "endgate/network/raknet/RakNetTypes.h"

namespace endgate {

class ProxyServer;

/**
 * The bytes RakNet stamps on every message it exchanges before a connection exists, chosen so that a
 * connected datagram cannot produce them by chance. Upstream keeps them file static in RakPeer.cpp,
 * which the port does not carry.
 *
 * @see OFFLINE_MESSAGE_DATA_ID
 */
inline constexpr std::array<std::byte, 16> OFFLINE_MESSAGE_DATA_ID{
    std::byte{0x00}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x00}, std::byte{0xFE}, std::byte{0xFE},
    std::byte{0xFE}, std::byte{0xFE}, std::byte{0xFD}, std::byte{0xFD}, std::byte{0xFD}, std::byte{0xFD},
    std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};

/**
 * Answers the RakNet messages that arrive before a connection exists. Only the unconnected ping is
 * answered; the rest of the handshake belongs to a session.
 *
 * @see RakNet::ProcessOfflineNetworkPacket
 */
class OfflineMessageHandler {
public:
    explicit OfflineMessageHandler(ProxyServer &server);

    /**
     * @see RakNetServerLocator::_setPingResponder
     */
    void setPingResponder(std::uint16_t port);

    [[nodiscard]] ByteBuf handle(std::span<const std::byte> datagram) const;

private:
    ProxyServer &server_;
    RakNet::RakNetGUID guid_;
    std::string offline_ping_response_;
};

}  // namespace endgate
