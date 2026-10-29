#include "endgate/network/raknet/offline.h"

#include <algorithm>

namespace endgate::raknet {

std::optional<OpenConnectionRequest2> OfflineHandler::handle(ByteBufView datagram, const SystemAddress &address,
                                                             bool allow_incoming_connections)
{
    if (datagram.readableBytes() <= 2 || !isOfflineMessage(datagram)) {
        return std::nullopt;
    }
    switch (static_cast<MessageId>(datagram.getUnsignedByte(datagram.readerIndex()))) {
    case MessageId::UnconnectedPing:
    case MessageId::UnconnectedPingOpenConnections: {
        const auto ping = UnconnectedPing::decode(datagram);
        if (!ping || (ping->id == MessageId::UnconnectedPingOpenConnections && !allow_incoming_connections)) {
            return std::nullopt;
        }
        auto buffer =
            Unpooled::buffer(1 + 8 + 8 + OFFLINE_MESSAGE_DATA_ID.size() + offline_ping_response_.readableBytes());
        UnconnectedPong{
            .send_ping_time = ping->send_ping_time, .server_guid = guid_, .data = offline_ping_response_.slice()}
            .encode(buffer);
        listener_.sendTo(address, buffer.slice());
        return std::nullopt;
    }
    case MessageId::OpenConnectionRequest1: {
        const auto request = OpenConnectionRequest1::decode(datagram);
        if (!request) {
            return std::nullopt;
        }
        auto buffer = Unpooled::buffer(64, 64);
        if (request->protocol != RAKNET_PROTOCOL_VERSION) {
            IncompatibleProtocolVersion{.protocol = RAKNET_PROTOCOL_VERSION, .server_guid = guid_}.encode(buffer);
        }
        else {
            const auto mtu = std::min<std::size_t>(request->mtu, MAXIMUM_MTU_SIZE);
            OpenConnectionReply1{.server_guid = guid_, .mtu = static_cast<std::uint16_t>(mtu)}.encode(buffer);
        }
        listener_.sendTo(address, buffer.slice());
        return std::nullopt;
    }
    case MessageId::OpenConnectionRequest2:
        return OpenConnectionRequest2::decode(datagram);
    default:
        return std::nullopt;
    }
}

void OfflineHandler::reject(const SystemAddress &address, MessageId reason)
{
    auto buffer = Unpooled::buffer(1 + OFFLINE_MESSAGE_DATA_ID.size() + 8);
    OfflineRejection{.id = reason, .server_guid = guid_}.encode(buffer);
    listener_.sendTo(address, buffer.slice());
}

}  // namespace endgate::raknet
