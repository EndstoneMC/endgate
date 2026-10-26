#include "endgate/network/raknet/offline.h"

#include <algorithm>
#include <array>

namespace endgate::raknet {

std::optional<OpenConnectionRequest2> OfflineHandler::handle(std::span<const std::byte> datagram,
                                                             const SystemAddress &address,
                                                             bool allow_incoming_connections)
{
    if (datagram.size() <= 2 || !isOfflineMessage(datagram)) {
        return std::nullopt;
    }
    switch (static_cast<MessageId>(datagram.front())) {
    case MessageId::UnconnectedPing:
    case MessageId::UnconnectedPingOpenConnections: {
        const auto ping = UnconnectedPing::decode(datagram);
        if (!ping || (ping->id == MessageId::UnconnectedPingOpenConnections && !allow_incoming_connections)) {
            return std::nullopt;
        }
        std::vector<std::byte> buffer(1 + 8 + 8 + OFFLINE_MESSAGE_DATA_ID.size() + offline_ping_response_.size());
        Writer writer{buffer};
        UnconnectedPong{.send_ping_time = ping->send_ping_time, .server_guid = guid_, .data = offline_ping_response_}
            .encode(writer);
        listener_.sendTo(address, writer.written());
        return std::nullopt;
    }
    case MessageId::OpenConnectionRequest1: {
        const auto request = OpenConnectionRequest1::decode(datagram);
        if (!request) {
            return std::nullopt;
        }
        std::array<std::byte, 64> buffer{};
        Writer writer{buffer};
        if (request->protocol != RAKNET_PROTOCOL_VERSION) {
            IncompatibleProtocolVersion{.protocol = RAKNET_PROTOCOL_VERSION, .server_guid = guid_}.encode(writer);
        }
        else {
            const auto mtu = std::min<std::size_t>(request->mtu, MAXIMUM_MTU_SIZE);
            OpenConnectionReply1{.server_guid = guid_, .mtu = static_cast<std::uint16_t>(mtu)}.encode(writer);
        }
        listener_.sendTo(address, writer.written());
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
    std::array<std::byte, 1 + OFFLINE_MESSAGE_DATA_ID.size() + 8> buffer{};
    Writer writer{buffer};
    OfflineRejection{.id = reason, .server_guid = guid_}.encode(writer);
    listener_.sendTo(address, writer.written());
}

}  // namespace endgate::raknet
