#include "endgate/network/offline.h"

#include <algorithm>
#include <format>
#include <random>
#include <string>

#include "endgate/network/legacy/raknet/BitStream.h"
#include "endgate/network/legacy/raknet/GetTime.h"
#include "endgate/network/legacy/raknet/MessageIdentifiers.h"
#include "endgate/network/ping.h"
#include "endgate/server.h"

namespace endgate {

OfflineMessageHandler::OfflineMessageHandler(ProxyServer &server)
    : server_(server), guid_(std::mt19937_64{std::random_device{}()}())
{
}

void OfflineMessageHandler::setPingResponder(std::uint16_t port)
{
    constexpr int joinable_through_server_screen = 1;
    constexpr int editor_world = 0;

    const auto ping = server_.getServerListPingHandler().getInitialPing();
    offline_ping_response_ = std::format(
        "MCPE;{};{};{};{};{};{};{};{};{};{};{};{};", ping.getDescription(), ping.getVersion().getProtocol(),
        ping.getVersion().getName(), ping.getPlayers().getOnline(), ping.getPlayers().getMax(), guid_.g,
        ping.getLevelName(), toString(ping.getGameType()), joinable_through_server_screen, port, port, editor_world);
}

ByteBuf OfflineMessageHandler::handle(std::span<const std::byte> datagram) const
{
    constexpr auto magic_offset = sizeof(RakNet::MessageID) + sizeof(RakNet::Time);

    if (datagram.size() < magic_offset + OFFLINE_MESSAGE_DATA_ID.size()) {
        return {};
    }
    const auto id = std::to_integer<std::uint8_t>(datagram.front());
    if (id != ID_UNCONNECTED_PING && id != ID_UNCONNECTED_PING_OPEN_CONNECTIONS) {
        return {};
    }
    if (!std::equal(OFFLINE_MESSAGE_DATA_ID.begin(), OFFLINE_MESSAGE_DATA_ID.end(), datagram.begin() + magic_offset)) {
        return {};
    }

    RakNet::BitStream in(reinterpret_cast<unsigned char *>(const_cast<std::byte *>(datagram.data())),
                         static_cast<unsigned int>(datagram.size()), false);
    in.IgnoreBits(8);
    RakNet::Time send_ping_time = 0;
    if (!in.Read(send_ping_time)) {
        return {};
    }

    RakNet::BitStream out;
    out.Write(static_cast<RakNet::MessageID>(ID_UNCONNECTED_PONG));
    out.Write(send_ping_time);
    out.Write(guid_);
    out.WriteAlignedBytes(reinterpret_cast<const unsigned char *>(OFFLINE_MESSAGE_DATA_ID.data()),
                          static_cast<unsigned int>(OFFLINE_MESSAGE_DATA_ID.size()));
    out.Write(static_cast<std::uint16_t>(offline_ping_response_.size()));
    out.Write(offline_ping_response_.data(), static_cast<unsigned int>(offline_ping_response_.size()));

    return ByteBuf::copyBuffer({reinterpret_cast<const std::byte *>(out.GetData()), out.GetNumberOfBytesUsed()});
}

}  // namespace endgate
