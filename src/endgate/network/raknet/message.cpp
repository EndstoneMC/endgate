#include "endgate/network/raknet/message.h"

#include <algorithm>
#include <limits>

namespace endgate::raknet {

bool isOfflineMessage(std::span<const std::byte> datagram)
{
    if (datagram.size() <= 2) {
        return true;
    }
    const auto magic_at = [datagram](std::size_t offset) {
        return datagram.size() >= offset + OFFLINE_MESSAGE_DATA_ID.size() &&
               std::ranges::equal(datagram.subspan(offset, OFFLINE_MESSAGE_DATA_ID.size()), OFFLINE_MESSAGE_DATA_ID);
    };
    constexpr std::size_t guid_and_magic =
        sizeof(std::uint8_t) + sizeof(std::uint64_t) + OFFLINE_MESSAGE_DATA_ID.size();
    switch (static_cast<MessageId>(datagram.front())) {
    case MessageId::UnconnectedPing:
    case MessageId::UnconnectedPingOpenConnections:
        return datagram.size() >= guid_and_magic && magic_at(9);
    case MessageId::UnconnectedPong:
        return magic_at(17);
    case MessageId::OutOfBandInternal:
        return datagram.size() >= guid_and_magic && magic_at(9);
    case MessageId::OpenConnectionReply1:
    case MessageId::OpenConnectionReply2:
    case MessageId::OpenConnectionRequest1:
    case MessageId::OpenConnectionRequest2:
    case MessageId::ConnectionAttemptFailed:
    case MessageId::NoFreeIncomingConnections:
    case MessageId::ConnectionBanned:
    case MessageId::AlreadyConnected:
    case MessageId::IpRecentlyConnected:
        return datagram.size() >= guid_and_magic && magic_at(1);
    case MessageId::IncompatibleProtocolVersion:
        return datagram.size() == guid_and_magic + 1 && magic_at(2);
    default:
        return false;
    }
}

std::optional<UnconnectedPing> UnconnectedPing::decode(std::span<const std::byte> message)
{
    Reader reader{message};
    const auto id = reader.readUint8();
    const auto send_ping_time = reader.readUint64();
    const auto magic = reader.readBytes(OFFLINE_MESSAGE_DATA_ID.size());
    if (!id || !send_ping_time || !magic || !std::ranges::equal(*magic, OFFLINE_MESSAGE_DATA_ID)) {
        return std::nullopt;
    }
    const auto ping_id = static_cast<MessageId>(*id);
    if (ping_id != MessageId::UnconnectedPing && ping_id != MessageId::UnconnectedPingOpenConnections) {
        return std::nullopt;
    }
    return UnconnectedPing{.id = ping_id,
                           .send_ping_time = *send_ping_time,
                           .client_guid = reader.readUint64().value_or(UNASSIGNED_RAKNET_GUID)};
}

void UnconnectedPing::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(id));
    writer.writeUint64(send_ping_time);
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeUint64(client_guid);
}

std::optional<UnconnectedPong> UnconnectedPong::decode(std::span<const std::byte> message)
{
    constexpr std::size_t max_offline_data_length = 400;

    Reader reader{message};
    const auto id = reader.readUint8();
    const auto send_ping_time = reader.readUint64();
    const auto server_guid = reader.readUint64();
    const auto magic = reader.readBytes(OFFLINE_MESSAGE_DATA_ID.size());
    if (!id || static_cast<MessageId>(*id) != MessageId::UnconnectedPong || !send_ping_time || !server_guid || !magic ||
        !std::ranges::equal(*magic, OFFLINE_MESSAGE_DATA_ID) || reader.remaining() >= max_offline_data_length) {
        return std::nullopt;
    }
    return UnconnectedPong{
        .send_ping_time = *send_ping_time, .server_guid = *server_guid, .data = reader.readRemaining()};
}

void UnconnectedPong::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::UnconnectedPong));
    writer.writeUint64(send_ping_time);
    writer.writeUint64(server_guid);
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeBytes(data);
}

std::optional<OpenConnectionRequest1> OpenConnectionRequest1::decode(std::span<const std::byte> message)
{
    if (!isOfflineMessage(message) || static_cast<MessageId>(message.front()) != MessageId::OpenConnectionRequest1) {
        return std::nullopt;
    }
    const auto mtu = std::min<std::size_t>(message.size() + UDP_HEADER_SIZE, std::numeric_limits<std::uint16_t>::max());
    return OpenConnectionRequest1{.protocol =
                                      std::to_integer<std::uint8_t>(message[1 + OFFLINE_MESSAGE_DATA_ID.size()]),
                                  .mtu = static_cast<std::uint16_t>(mtu)};
}

void OpenConnectionRequest1::encode(Writer &writer) const
{
    const auto start = writer.size();
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::OpenConnectionRequest1));
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeUint8(protocol);
    const auto padded_length = std::max<std::size_t>(mtu, UDP_HEADER_SIZE) - UDP_HEADER_SIZE;
    const auto written = writer.size() - start;
    writer.writeZeros(padded_length > written ? padded_length - written : 0);
}

std::optional<OpenConnectionReply1> OpenConnectionReply1::decode(std::span<const std::byte> message)
{
    if (!isOfflineMessage(message) || static_cast<MessageId>(message.front()) != MessageId::OpenConnectionReply1) {
        return std::nullopt;
    }
    Reader reader{message.subspan(1 + OFFLINE_MESSAGE_DATA_ID.size())};
    const auto server_guid = reader.readUint64();
    const auto security = reader.readUint8();
    if (!server_guid || !security) {
        return std::nullopt;
    }
    OpenConnectionReply1 reply{.server_guid = *server_guid};
    if (*security != 0) {
        reply.cookie = reader.readUint32();
        if (!reply.cookie) {
            return std::nullopt;
        }
    }
    const auto mtu = reader.readUint16();
    if (!mtu) {
        return std::nullopt;
    }
    reply.mtu = *mtu;
    return reply;
}

void OpenConnectionReply1::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::OpenConnectionReply1));
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeUint64(server_guid);
    writer.writeUint8(cookie ? 1 : 0);
    if (cookie) {
        writer.writeUint32(*cookie);
    }
    writer.writeUint16(mtu);
}

std::optional<OpenConnectionRequest2> OpenConnectionRequest2::decode(std::span<const std::byte> message)
{
    if (!isOfflineMessage(message) || static_cast<MessageId>(message.front()) != MessageId::OpenConnectionRequest2) {
        return std::nullopt;
    }
    Reader reader{message.subspan(1 + OFFLINE_MESSAGE_DATA_ID.size())};
    const auto server_address = reader.readAddress();
    const auto mtu = reader.readUint16();
    if (!server_address || !mtu) {
        return std::nullopt;
    }
    return OpenConnectionRequest2{.server_address = *server_address,
                                  .mtu = *mtu,
                                  .client_guid = reader.readUint64().value_or(UNASSIGNED_RAKNET_GUID)};
}

void OpenConnectionRequest2::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::OpenConnectionRequest2));
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    if (cookie) {
        writer.writeUint32(*cookie);
        writer.writeUint8(0);
    }
    writer.writeAddress(server_address);
    writer.writeUint16(mtu);
    writer.writeUint64(client_guid);
}

std::optional<OpenConnectionReply2> OpenConnectionReply2::decode(std::span<const std::byte> message)
{
    if (!isOfflineMessage(message) || static_cast<MessageId>(message.front()) != MessageId::OpenConnectionReply2) {
        return std::nullopt;
    }
    Reader reader{message.subspan(1 + OFFLINE_MESSAGE_DATA_ID.size())};
    const auto server_guid = reader.readUint64();
    const auto client_address = reader.readAddress();
    const auto mtu = reader.readUint16();
    if (!server_guid || !client_address || !mtu) {
        return std::nullopt;
    }
    const auto security = reader.readUint8().value_or(0);
    return OpenConnectionReply2{.server_guid = *server_guid,
                                .client_address = *client_address,
                                .mtu = *mtu,
                                .security = (security & 0x80U) != 0};
}

void OpenConnectionReply2::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::OpenConnectionReply2));
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeUint64(server_guid);
    writer.writeAddress(client_address);
    writer.writeUint16(mtu);
    writer.writeUint8(security ? 0x80 : 0x00);
}

std::optional<IncompatibleProtocolVersion> IncompatibleProtocolVersion::decode(std::span<const std::byte> message)
{
    if (!isOfflineMessage(message) ||
        static_cast<MessageId>(message.front()) != MessageId::IncompatibleProtocolVersion) {
        return std::nullopt;
    }
    Reader reader{message.subspan(1)};
    const auto protocol = reader.readUint8();
    const auto server_guid = Reader{message.subspan(2 + OFFLINE_MESSAGE_DATA_ID.size())}.readUint64();
    if (!protocol || !server_guid) {
        return std::nullopt;
    }
    return IncompatibleProtocolVersion{.protocol = *protocol, .server_guid = *server_guid};
}

void IncompatibleProtocolVersion::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::IncompatibleProtocolVersion));
    writer.writeUint8(protocol);
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeUint64(server_guid);
}

std::optional<OfflineRejection> OfflineRejection::decode(std::span<const std::byte> message)
{
    if (!isOfflineMessage(message)) {
        return std::nullopt;
    }
    const auto id = static_cast<MessageId>(message.front());
    if (id != MessageId::AlreadyConnected && id != MessageId::NoFreeIncomingConnections &&
        id != MessageId::ConnectionBanned && id != MessageId::IpRecentlyConnected &&
        id != MessageId::ConnectionAttemptFailed) {
        return std::nullopt;
    }
    const auto server_guid = Reader{message.subspan(1 + OFFLINE_MESSAGE_DATA_ID.size())}.readUint64();
    if (!server_guid) {
        return std::nullopt;
    }
    return OfflineRejection{.id = id, .server_guid = *server_guid};
}

void OfflineRejection::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(id));
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeUint64(server_guid);
}

std::optional<ConnectedPing> ConnectedPing::decode(std::span<const std::byte> message)
{
    Reader reader{message};
    const auto id = reader.readUint8();
    const auto send_ping_time = reader.readUint64();
    if (!id || static_cast<MessageId>(*id) != MessageId::ConnectedPing || !send_ping_time || reader.remaining() != 0) {
        return std::nullopt;
    }
    return ConnectedPing{.send_ping_time = *send_ping_time};
}

void ConnectedPing::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::ConnectedPing));
    writer.writeUint64(send_ping_time);
}

std::optional<ConnectedPong> ConnectedPong::decode(std::span<const std::byte> message)
{
    Reader reader{message};
    const auto id = reader.readUint8();
    const auto send_ping_time = reader.readUint64();
    const auto send_pong_time = reader.readUint64();
    if (!id || static_cast<MessageId>(*id) != MessageId::ConnectedPong || !send_ping_time || !send_pong_time ||
        reader.remaining() != 0) {
        return std::nullopt;
    }
    return ConnectedPong{.send_ping_time = *send_ping_time, .send_pong_time = *send_pong_time};
}

void ConnectedPong::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::ConnectedPong));
    writer.writeUint64(send_ping_time);
    writer.writeUint64(send_pong_time);
}

std::optional<ConnectionRequest> ConnectionRequest::decode(std::span<const std::byte> message)
{
    Reader reader{message};
    const auto id = reader.readUint8();
    const auto client_guid = reader.readUint64();
    const auto send_ping_time = reader.readUint64();
    if (!id || static_cast<MessageId>(*id) != MessageId::ConnectionRequest || !client_guid || !send_ping_time) {
        return std::nullopt;
    }
    const auto security = reader.readUint8();
    return ConnectionRequest{.client_guid = *client_guid,
                             .send_ping_time = *send_ping_time,
                             .security = security.value_or(0) != 0,
                             .password = reader.readRemaining()};
}

void ConnectionRequest::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::ConnectionRequest));
    writer.writeUint64(client_guid);
    writer.writeUint64(send_ping_time);
    writer.writeUint8(security ? 1 : 0);
    writer.writeBytes(password);
}

std::optional<ConnectionRequestAccepted> ConnectionRequestAccepted::decode(std::span<const std::byte> message)
{
    constexpr std::size_t minimum_size = sizeof(std::uint8_t) + sizeof(std::uint32_t) + sizeof(std::uint16_t) +
                                         sizeof(std::uint16_t) + 2 * sizeof(std::uint64_t);

    if (message.size() <= minimum_size ||
        static_cast<MessageId>(message.front()) != MessageId::ConnectionRequestAccepted) {
        return std::nullopt;
    }
    Reader reader{message.subspan(1)};
    ConnectionRequestAccepted accepted;
    const auto client_address = reader.readAddress();
    if (!client_address) {
        return accepted;
    }
    accepted.client_address = *client_address;
    const auto system_index = reader.readUint16();
    if (!system_index) {
        return accepted;
    }
    accepted.system_index = *system_index;
    for (auto &internal_address : accepted.internal_addresses) {
        const auto address = reader.readAddress();
        if (!address) {
            return accepted;
        }
        internal_address = *address;
    }
    accepted.send_ping_time = reader.readUint64().value_or(0);
    accepted.send_pong_time = reader.readUint64().value_or(0);
    return accepted;
}

void ConnectionRequestAccepted::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::ConnectionRequestAccepted));
    writer.writeAddress(client_address);
    writer.writeUint16(system_index);
    for (const auto &internal_address : internal_addresses) {
        writer.writeAddress(internal_address);
    }
    writer.writeUint64(send_ping_time);
    writer.writeUint64(send_pong_time);
}

std::optional<NewIncomingConnection> NewIncomingConnection::decode(std::span<const std::byte> message)
{
    constexpr std::size_t minimum_size =
        sizeof(std::uint8_t) + sizeof(std::uint32_t) + sizeof(std::uint16_t) + 2 * sizeof(std::uint64_t);

    if (message.size() <= minimum_size || static_cast<MessageId>(message.front()) != MessageId::NewIncomingConnection) {
        return std::nullopt;
    }
    Reader reader{message.subspan(1)};
    NewIncomingConnection incoming;
    const auto server_address = reader.readAddress();
    if (!server_address) {
        return incoming;
    }
    incoming.server_address = *server_address;
    for (auto &internal_address : incoming.internal_addresses) {
        const auto address = reader.readAddress();
        if (!address) {
            return incoming;
        }
        internal_address = *address;
    }
    incoming.send_ping_time = reader.readUint64().value_or(0);
    incoming.send_pong_time = reader.readUint64().value_or(0);
    return incoming;
}

void NewIncomingConnection::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::NewIncomingConnection));
    writer.writeAddress(server_address);
    for (const auto &internal_address : internal_addresses) {
        writer.writeAddress(internal_address);
    }
    writer.writeUint64(send_ping_time);
    writer.writeUint64(send_pong_time);
}

void InvalidPassword::encode(Writer &writer) const
{
    writer.writeUint8(static_cast<std::uint8_t>(MessageId::InvalidPassword));
    writer.writeUint64(server_guid);
}

}  // namespace endgate::raknet
