#include "endgate/network/raknet/message.h"

#include <algorithm>
#include <limits>

namespace endgate::raknet {

bool isOfflineMessage(const ByteBufView &datagram)
{
    const auto size = datagram.readableBytes();
    if (size <= 2) {
        return true;
    }
    const auto start = datagram.readerIndex();
    const auto magic_at = [&datagram, size, start](std::size_t offset) {
        return size >= offset + OFFLINE_MESSAGE_DATA_ID.size() &&
               std::ranges::equal(datagram.nioBuffer(start + offset, OFFLINE_MESSAGE_DATA_ID.size()),
                                  OFFLINE_MESSAGE_DATA_ID);
    };
    constexpr std::size_t guid_and_magic =
        sizeof(std::uint8_t) + sizeof(std::uint64_t) + OFFLINE_MESSAGE_DATA_ID.size();
    switch (static_cast<MessageId>(datagram.getUnsignedByte(start))) {
    case MessageId::UnconnectedPing:
    case MessageId::UnconnectedPingOpenConnections:
        return size >= guid_and_magic && magic_at(9);
    case MessageId::UnconnectedPong:
        return magic_at(17);
    case MessageId::OutOfBandInternal:
        return size >= guid_and_magic && magic_at(9);
    case MessageId::OpenConnectionReply1:
    case MessageId::OpenConnectionReply2:
    case MessageId::OpenConnectionRequest1:
    case MessageId::OpenConnectionRequest2:
    case MessageId::ConnectionAttemptFailed:
    case MessageId::NoFreeIncomingConnections:
    case MessageId::ConnectionBanned:
    case MessageId::AlreadyConnected:
    case MessageId::IpRecentlyConnected:
        return size >= guid_and_magic && magic_at(1);
    case MessageId::IncompatibleProtocolVersion:
        return size == guid_and_magic + 1 && magic_at(2);
    default:
        return false;
    }
}

std::optional<UnconnectedPing> UnconnectedPing::decode(ByteBufView message)
{
    if (!message.isReadable(1 + 8 + OFFLINE_MESSAGE_DATA_ID.size())) {
        return std::nullopt;
    }
    const auto id = message.readUnsignedByte();
    const auto send_ping_time = static_cast<std::uint64_t>(message.readLong());
    const auto magic = message.readSlice(OFFLINE_MESSAGE_DATA_ID.size());
    if (!std::ranges::equal(magic.nioBuffer(), OFFLINE_MESSAGE_DATA_ID)) {
        return std::nullopt;
    }
    const auto ping_id = static_cast<MessageId>(id);
    if (ping_id != MessageId::UnconnectedPing && ping_id != MessageId::UnconnectedPingOpenConnections) {
        return std::nullopt;
    }
    return UnconnectedPing{.id = ping_id,
                           .send_ping_time = send_ping_time,
                           .client_guid = message.isReadable(8) ? static_cast<std::uint64_t>(message.readLong())
                                                                : UNASSIGNED_RAKNET_GUID};
}

void UnconnectedPing::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(id));
    writer.writeLong(send_ping_time);
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeLong(client_guid);
}

std::optional<UnconnectedPong> UnconnectedPong::decode(ByteBufView message)
{
    constexpr std::size_t max_offline_data_length = 400;

    if (!message.isReadable(1 + 8 + 8 + OFFLINE_MESSAGE_DATA_ID.size())) {
        return std::nullopt;
    }
    const auto id = message.readUnsignedByte();
    const auto send_ping_time = static_cast<std::uint64_t>(message.readLong());
    const auto server_guid = static_cast<std::uint64_t>(message.readLong());
    const auto magic = message.readSlice(OFFLINE_MESSAGE_DATA_ID.size());
    if (static_cast<MessageId>(id) != MessageId::UnconnectedPong ||
        !std::ranges::equal(magic.nioBuffer(), OFFLINE_MESSAGE_DATA_ID) ||
        message.readableBytes() >= max_offline_data_length) {
        return std::nullopt;
    }
    return UnconnectedPong{.send_ping_time = send_ping_time, .server_guid = server_guid, .data = message.slice()};
}

void UnconnectedPong::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(MessageId::UnconnectedPong));
    writer.writeLong(send_ping_time);
    writer.writeLong(server_guid);
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeBytes(data.nioBuffer());
}

std::optional<OpenConnectionRequest1> OpenConnectionRequest1::decode(ByteBufView message)
{
    if (message.readableBytes() <= 2 || !isOfflineMessage(message) ||
        static_cast<MessageId>(message.getUnsignedByte(message.readerIndex())) != MessageId::OpenConnectionRequest1) {
        return std::nullopt;
    }
    const auto mtu =
        std::min<std::size_t>(message.readableBytes() + UDP_HEADER_SIZE, std::numeric_limits<std::uint16_t>::max());
    return OpenConnectionRequest1{
        .protocol = message.getUnsignedByte(message.readerIndex() + 1 + OFFLINE_MESSAGE_DATA_ID.size()),
        .mtu = static_cast<std::uint16_t>(mtu)};
}

void OpenConnectionRequest1::encode(ByteBuf &writer) const
{
    const auto start = writer.writerIndex();
    writer.writeByte(static_cast<std::uint8_t>(MessageId::OpenConnectionRequest1));
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeByte(protocol);
    const auto padded_length = std::max<std::size_t>(mtu, UDP_HEADER_SIZE) - UDP_HEADER_SIZE;
    const auto written = writer.writerIndex() - start;
    writer.writeZero(padded_length > written ? padded_length - written : 0);
}

std::optional<OpenConnectionReply1> OpenConnectionReply1::decode(ByteBufView message)
{
    if (message.readableBytes() <= 2 || !isOfflineMessage(message) ||
        static_cast<MessageId>(message.getUnsignedByte(message.readerIndex())) != MessageId::OpenConnectionReply1) {
        return std::nullopt;
    }
    message.skipBytes(1 + OFFLINE_MESSAGE_DATA_ID.size());
    if (!message.isReadable(8 + 1)) {
        return std::nullopt;
    }
    OpenConnectionReply1 reply{.server_guid = static_cast<std::uint64_t>(message.readLong())};
    const auto security = message.readUnsignedByte();
    if (security != 0) {
        if (!message.isReadable(4)) {
            return std::nullopt;
        }
        reply.cookie = message.readUnsignedInt();
    }
    if (!message.isReadable(2)) {
        return std::nullopt;
    }
    reply.mtu = message.readUnsignedShort();
    return reply;
}

void OpenConnectionReply1::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(MessageId::OpenConnectionReply1));
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeLong(server_guid);
    writer.writeByte(cookie ? 1 : 0);
    if (cookie) {
        writer.writeInt(*cookie);
    }
    writer.writeShort(mtu);
}

std::optional<OpenConnectionRequest2> OpenConnectionRequest2::decode(ByteBufView message)
{
    if (message.readableBytes() <= 2 || !isOfflineMessage(message) ||
        static_cast<MessageId>(message.getUnsignedByte(message.readerIndex())) != MessageId::OpenConnectionRequest2) {
        return std::nullopt;
    }
    message.skipBytes(1 + OFFLINE_MESSAGE_DATA_ID.size());
    if (!isAddressReadable(message)) {
        return std::nullopt;
    }
    const auto server_address = readAddress(message);
    if (!message.isReadable(2)) {
        return std::nullopt;
    }
    const auto mtu = message.readUnsignedShort();
    return OpenConnectionRequest2{.server_address = server_address,
                                  .mtu = mtu,
                                  .client_guid = message.isReadable(8) ? static_cast<std::uint64_t>(message.readLong())
                                                                       : UNASSIGNED_RAKNET_GUID};
}

void OpenConnectionRequest2::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(MessageId::OpenConnectionRequest2));
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    if (cookie) {
        writer.writeInt(*cookie);
        writer.writeByte(0);
    }
    writeAddress(writer, server_address);
    writer.writeShort(mtu);
    writer.writeLong(client_guid);
}

std::optional<OpenConnectionReply2> OpenConnectionReply2::decode(ByteBufView message)
{
    if (message.readableBytes() <= 2 || !isOfflineMessage(message) ||
        static_cast<MessageId>(message.getUnsignedByte(message.readerIndex())) != MessageId::OpenConnectionReply2) {
        return std::nullopt;
    }
    message.skipBytes(1 + OFFLINE_MESSAGE_DATA_ID.size());
    if (!message.isReadable(8)) {
        return std::nullopt;
    }
    const auto server_guid = static_cast<std::uint64_t>(message.readLong());
    if (!isAddressReadable(message)) {
        return std::nullopt;
    }
    const auto client_address = readAddress(message);
    if (!message.isReadable(2)) {
        return std::nullopt;
    }
    const auto mtu = message.readUnsignedShort();
    const auto security = message.isReadable() ? message.readUnsignedByte() : 0;
    return OpenConnectionReply2{
        .server_guid = server_guid, .client_address = client_address, .mtu = mtu, .security = (security & 0x80U) != 0};
}

void OpenConnectionReply2::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(MessageId::OpenConnectionReply2));
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeLong(server_guid);
    writeAddress(writer, client_address);
    writer.writeShort(mtu);
    writer.writeByte(security ? 0x80 : 0x00);
}

std::optional<IncompatibleProtocolVersion> IncompatibleProtocolVersion::decode(ByteBufView message)
{
    if (message.readableBytes() <= 2 || !isOfflineMessage(message) ||
        static_cast<MessageId>(message.getUnsignedByte(message.readerIndex())) !=
            MessageId::IncompatibleProtocolVersion) {
        return std::nullopt;
    }
    message.skipBytes(1);
    const auto protocol = message.readUnsignedByte();
    message.skipBytes(OFFLINE_MESSAGE_DATA_ID.size());
    if (!message.isReadable(8)) {
        return std::nullopt;
    }
    return IncompatibleProtocolVersion{.protocol = protocol,
                                       .server_guid = static_cast<std::uint64_t>(message.readLong())};
}

void IncompatibleProtocolVersion::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(MessageId::IncompatibleProtocolVersion));
    writer.writeByte(protocol);
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeLong(server_guid);
}

std::optional<OfflineRejection> OfflineRejection::decode(ByteBufView message)
{
    if (message.readableBytes() <= 2 || !isOfflineMessage(message)) {
        return std::nullopt;
    }
    const auto id = static_cast<MessageId>(message.readUnsignedByte());
    if (id != MessageId::AlreadyConnected && id != MessageId::NoFreeIncomingConnections &&
        id != MessageId::ConnectionBanned && id != MessageId::IpRecentlyConnected &&
        id != MessageId::ConnectionAttemptFailed) {
        return std::nullopt;
    }
    message.skipBytes(OFFLINE_MESSAGE_DATA_ID.size());
    if (!message.isReadable(8)) {
        return std::nullopt;
    }
    return OfflineRejection{.id = id, .server_guid = static_cast<std::uint64_t>(message.readLong())};
}

void OfflineRejection::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(id));
    writer.writeBytes(OFFLINE_MESSAGE_DATA_ID);
    writer.writeLong(server_guid);
}

std::optional<ConnectedPing> ConnectedPing::decode(ByteBufView message)
{
    if (message.readableBytes() != 1 + 8 ||
        static_cast<MessageId>(message.readUnsignedByte()) != MessageId::ConnectedPing) {
        return std::nullopt;
    }
    return ConnectedPing{.send_ping_time = static_cast<std::uint64_t>(message.readLong())};
}

void ConnectedPing::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(MessageId::ConnectedPing));
    writer.writeLong(send_ping_time);
}

std::optional<ConnectedPong> ConnectedPong::decode(ByteBufView message)
{
    if (message.readableBytes() != 1 + 8 + 8 ||
        static_cast<MessageId>(message.readUnsignedByte()) != MessageId::ConnectedPong) {
        return std::nullopt;
    }
    const auto send_ping_time = static_cast<std::uint64_t>(message.readLong());
    return ConnectedPong{.send_ping_time = send_ping_time,
                         .send_pong_time = static_cast<std::uint64_t>(message.readLong())};
}

void ConnectedPong::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(MessageId::ConnectedPong));
    writer.writeLong(send_ping_time);
    writer.writeLong(send_pong_time);
}

std::optional<ConnectionRequest> ConnectionRequest::decode(ByteBufView message)
{
    if (!message.isReadable(1 + 8 + 8) ||
        static_cast<MessageId>(message.readUnsignedByte()) != MessageId::ConnectionRequest) {
        return std::nullopt;
    }
    const auto client_guid = static_cast<std::uint64_t>(message.readLong());
    const auto send_ping_time = static_cast<std::uint64_t>(message.readLong());
    const auto security = message.isReadable() ? message.readUnsignedByte() : 0;
    return ConnectionRequest{.client_guid = client_guid,
                             .send_ping_time = send_ping_time,
                             .security = security != 0,
                             .password = message.slice()};
}

void ConnectionRequest::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(MessageId::ConnectionRequest));
    writer.writeLong(client_guid);
    writer.writeLong(send_ping_time);
    writer.writeByte(security ? 1 : 0);
    writer.writeBytes(password.nioBuffer());
}

std::optional<ConnectionRequestAccepted> ConnectionRequestAccepted::decode(ByteBufView message)
{
    constexpr std::size_t minimum_size = sizeof(std::uint8_t) + sizeof(std::uint32_t) + sizeof(std::uint16_t) +
                                         sizeof(std::uint16_t) + 2 * sizeof(std::uint64_t);

    if (message.readableBytes() <= minimum_size ||
        static_cast<MessageId>(message.readUnsignedByte()) != MessageId::ConnectionRequestAccepted) {
        return std::nullopt;
    }
    ConnectionRequestAccepted accepted;
    if (!isAddressReadable(message)) {
        return accepted;
    }
    accepted.client_address = readAddress(message);
    if (!message.isReadable(2)) {
        return accepted;
    }
    accepted.system_index = message.readUnsignedShort();
    for (auto &internal_address : accepted.internal_addresses) {
        if (!isAddressReadable(message)) {
            return accepted;
        }
        internal_address = readAddress(message);
    }
    accepted.send_ping_time = message.isReadable(8) ? static_cast<std::uint64_t>(message.readLong()) : 0;
    accepted.send_pong_time = message.isReadable(8) ? static_cast<std::uint64_t>(message.readLong()) : 0;
    return accepted;
}

void ConnectionRequestAccepted::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(MessageId::ConnectionRequestAccepted));
    writeAddress(writer, client_address);
    writer.writeShort(system_index);
    for (const auto &internal_address : internal_addresses) {
        writeAddress(writer, internal_address);
    }
    writer.writeLong(send_ping_time);
    writer.writeLong(send_pong_time);
}

std::optional<NewIncomingConnection> NewIncomingConnection::decode(ByteBufView message)
{
    constexpr std::size_t minimum_size =
        sizeof(std::uint8_t) + sizeof(std::uint32_t) + sizeof(std::uint16_t) + 2 * sizeof(std::uint64_t);

    if (message.readableBytes() <= minimum_size ||
        static_cast<MessageId>(message.readUnsignedByte()) != MessageId::NewIncomingConnection) {
        return std::nullopt;
    }
    NewIncomingConnection incoming;
    if (!isAddressReadable(message)) {
        return incoming;
    }
    incoming.server_address = readAddress(message);
    for (auto &internal_address : incoming.internal_addresses) {
        if (!isAddressReadable(message)) {
            return incoming;
        }
        internal_address = readAddress(message);
    }
    incoming.send_ping_time = message.isReadable(8) ? static_cast<std::uint64_t>(message.readLong()) : 0;
    incoming.send_pong_time = message.isReadable(8) ? static_cast<std::uint64_t>(message.readLong()) : 0;
    return incoming;
}

void NewIncomingConnection::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(MessageId::NewIncomingConnection));
    writeAddress(writer, server_address);
    for (const auto &internal_address : internal_addresses) {
        writeAddress(writer, internal_address);
    }
    writer.writeLong(send_ping_time);
    writer.writeLong(send_pong_time);
}

void InvalidPassword::encode(ByteBuf &writer) const
{
    writer.writeByte(static_cast<std::uint8_t>(MessageId::InvalidPassword));
    writer.writeLong(server_guid);
}

}  // namespace endgate::raknet
