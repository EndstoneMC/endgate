#include "endgate/network/raknet/connection.h"

#include <algorithm>
#include <array>
#include <utility>

namespace endgate::raknet {

namespace {

constexpr auto CONNECTION_REQUEST_TIMEOUT = std::chrono::milliseconds{10000};
constexpr auto PING_INTERVAL = std::chrono::milliseconds{5000};
constexpr std::size_t HANDSHAKE_HELD_BYTES = 64 * 1024;

std::size_t clampMtu(std::size_t mtu)
{
    return std::clamp(mtu, MINIMUM_MTU_SIZE, MAXIMUM_MTU_SIZE);
}

}  // namespace

Connection::Connection(Listener &listener, ConnectionConfiguration configuration, SystemAddress system_address,
                       const OpenConnectionRequest2 &request, std::uint16_t system_index, TimePoint now)
    : listener_(listener), configuration_(std::move(configuration)), system_address_(std::move(system_address)),
      guid_(request.client_guid), system_index_(system_index), we_initiated_the_connection_(false),
      connect_mode_(ConnectMode::UnverifiedSender), mtu_size_(clampMtu(request.mtu)), connection_time_(now),
      last_reliable_send_(now), next_ping_time_(now), next_request_time_(now)
{
    reliability_layer_.emplace(static_cast<ReliabilityLayer::Listener &>(*this), mtu_size_, now);
    reliability_layer_->setTimeoutTime(configuration_.timeout_time);
    reliability_layer_->setUnreliableTimeout(configuration_.unreliable_timeout);
    reliability_layer_->setMaximumHeldBytes(HANDSHAKE_HELD_BYTES);
}

Connection::Connection(Listener &listener, ConnectionConfiguration configuration, SystemAddress system_address,
                       TimePoint now)
    : listener_(listener), configuration_(std::move(configuration)), system_address_(std::move(system_address)),
      we_initiated_the_connection_(true), connect_mode_(ConnectMode::RequestingOpenConnection), connection_time_(now),
      last_reliable_send_(now), next_ping_time_(now), next_request_time_(now)
{
}

std::uint64_t Connection::getTime(TimePoint now)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

void Connection::sendDatagram(ByteBufView datagram)
{
    listener_.sendDatagram(datagram);
}

void Connection::onReliabilityLayerNotification(std::string_view message, bool is_error)
{
    listener_.onReliabilityLayerNotification(message, is_error);
}

void Connection::handleDatagram(ByteBufView datagram, TimePoint now)
{
    if (isClosed()) {
        return;
    }
    if (isOfflineMessage(datagram)) {
        handleOfflineMessage(datagram, now);
        return;
    }
    if (!reliability_layer_) {
        return;
    }
    delivering_ = true;
    reliability_layer_->handleSocketReceiveFromConnectedPlayer(datagram, now);
    delivering_ = false;
    update(now);
}

void Connection::handleTimeout(TimePoint now)
{
    update(now);
}

void Connection::handleOfflineMessage(ByteBufView datagram, TimePoint now)
{
    if (datagram.readableBytes() <= 2) {
        return;
    }
    const auto id = static_cast<MessageId>(datagram.getUnsignedByte(datagram.readerIndex()));
    if (id == MessageId::OpenConnectionRequest2) {
        const auto request = OpenConnectionRequest2::decode(datagram);
        if (!request) {
            return;
        }
        auto buffer = Unpooled::buffer(MAXIMUM_MTU_SIZE, MAXIMUM_MTU_SIZE);
        if (!we_initiated_the_connection_ && connect_mode_ == ConnectMode::UnverifiedSender &&
            request->client_guid == guid_) {
            OpenConnectionReply2{
                .server_guid = configuration_.guid, .client_address = system_address_, .mtu = request->mtu}
                .encode(buffer);
        }
        else {
            OfflineRejection{.id = MessageId::AlreadyConnected, .server_guid = configuration_.guid}.encode(buffer);
        }
        listener_.sendDatagram(buffer.slice());
        return;
    }
    if (connect_mode_ != ConnectMode::RequestingOpenConnection) {
        return;
    }

    if (id == MessageId::OpenConnectionReply1) {
        const auto reply = OpenConnectionReply1::decode(datagram);
        if (!reply) {
            return;
        }
        auto buffer = Unpooled::buffer(MAXIMUM_MTU_SIZE, MAXIMUM_MTU_SIZE);
        OpenConnectionRequest2{.cookie = reply->cookie,
                               .server_address = system_address_,
                               .mtu = reply->mtu,
                               .client_guid = configuration_.guid}
            .encode(buffer);
        listener_.sendDatagram(buffer.slice());
        return;
    }

    if (id == MessageId::OpenConnectionReply2) {
        const auto reply = OpenConnectionReply2::decode(datagram);
        if (!reply) {
            return;
        }
        guid_ = reply->server_guid;
        my_external_system_address_ = reply->client_address;
        mtu_size_ = clampMtu(reply->mtu);
        reliability_layer_.emplace(static_cast<ReliabilityLayer::Listener &>(*this), mtu_size_, now);
        reliability_layer_->setTimeoutTime(configuration_.timeout_time);
        reliability_layer_->setUnreliableTimeout(configuration_.unreliable_timeout);
        reliability_layer_->setMaximumHeldBytes(HANDSHAKE_HELD_BYTES);
        connect_mode_ = ConnectMode::RequestedConnection;
        connection_time_ = now;
        last_reliable_send_ = now;
        next_ping_time_ = now;

        auto buffer = Unpooled::buffer(MAXIMUM_MTU_SIZE, MAXIMUM_MTU_SIZE);
        ConnectionRequest{.client_guid = configuration_.guid,
                          .send_ping_time = getTime(now),
                          .password = configuration_.password.slice()}
            .encode(buffer);
        sendImmediate(buffer.slice(), PacketPriority::Immediate, PacketReliability::Reliable, now);
        update(now);
        return;
    }

    if (const auto incompatible = IncompatibleProtocolVersion::decode(datagram)) {
        closeConnectionInternal(DisconnectReason::IncompatibleProtocolVersion);
        return;
    }
    if (const auto rejection = OfflineRejection::decode(datagram)) {
        switch (rejection->id) {
        case MessageId::AlreadyConnected:
            closeConnectionInternal(DisconnectReason::AlreadyConnected);
            return;
        case MessageId::NoFreeIncomingConnections:
            closeConnectionInternal(DisconnectReason::NoFreeIncomingConnections);
            return;
        case MessageId::ConnectionBanned:
            closeConnectionInternal(DisconnectReason::ConnectionBanned);
            return;
        case MessageId::IpRecentlyConnected:
            closeConnectionInternal(DisconnectReason::IpRecentlyConnected);
            return;
        default:
            closeConnectionInternal(DisconnectReason::ConnectionAttemptFailed);
            return;
        }
    }
}

void Connection::onMessage(ByteBufView message, TimePoint now)
{
    if (isClosed() || !reliability_layer_) {
        return;
    }
    auto &reliability_layer = *reliability_layer_;
    const auto first = message.getUnsignedByte(message.readerIndex());
    const auto id = static_cast<MessageId>(first);
    if (id == MessageId::ConnectionAttemptFailed) {
        return;
    }

    const auto is_user_message = first >= static_cast<std::uint8_t>(MessageId::Timestamp) ||
                                 id == MessageId::SndReceiptAcked || id == MessageId::SndReceiptLoss;
    if (connect_mode_ != ConnectMode::UnverifiedSender && is_user_message) {
        listener_.onMessage(message);
        return;
    }

    const auto bytes = message;

    if (connect_mode_ == ConnectMode::UnverifiedSender) {
        if (id == MessageId::ConnectionRequest) {
            parseConnectionRequestPacket(bytes, now);
            return;
        }
        closeConnectionInternal(DisconnectReason::Rejected);
        return;
    }

    switch (id) {
    case MessageId::ConnectionRequest: {
        if (connect_mode_ == ConnectMode::RequestedConnection) {
            parseConnectionRequestPacket(bytes, now);
            return;
        }
        if (reliability_layer.isOutgoingDataWaiting()) {
            return;
        }
        const auto request = ConnectionRequest::decode(bytes);
        onConnectionRequest(request ? request->send_ping_time : 0, now);
        return;
    }
    case MessageId::NewIncomingConnection: {
        const auto incoming = NewIncomingConnection::decode(bytes);
        if (!incoming || connect_mode_ != ConnectMode::HandlingConnectionRequest) {
            return;
        }
        connect_mode_ = ConnectMode::Connected;
        reliability_layer.setMaximumHeldBytes(configuration_.maximum_held_bytes);
        pingInternal(PacketReliability::Unreliable, now);
        their_internal_system_address_ = incoming->internal_addresses;
        onConnectedPong(incoming->send_ping_time, now);
        my_external_system_address_ = incoming->server_address;
        announce();
        return;
    }
    case MessageId::ConnectedPong: {
        if (const auto pong = ConnectedPong::decode(bytes)) {
            onConnectedPong(pong->send_ping_time, now);
        }
        return;
    }
    case MessageId::ConnectedPing: {
        const auto ping = ConnectedPing::decode(bytes);
        if (!ping) {
            return;
        }
        auto buffer = Unpooled::buffer(17, 17);
        ConnectedPong{.send_ping_time = ping->send_ping_time, .send_pong_time = getTime(now)}.encode(buffer);
        sendImmediate(buffer.slice(), PacketPriority::Immediate, PacketReliability::Unreliable, now);
        return;
    }
    case MessageId::DisconnectionNotification:
        connect_mode_ = ConnectMode::DisconnectOnNoAck;
        return;
    case MessageId::InvalidPassword:
        if (connect_mode_ == ConnectMode::RequestedConnection) {
            silent_disconnect_reason_ = DisconnectReason::InvalidPassword;
            connect_mode_ = ConnectMode::DisconnectAsapSilently;
        }
        return;
    case MessageId::ConnectionRequestAccepted: {
        const auto accepted = ConnectionRequestAccepted::decode(bytes);
        const bool allow_connection = connect_mode_ == ConnectMode::HandlingConnectionRequest ||
                                      connect_mode_ == ConnectMode::RequestedConnection;
        if (!accepted || !allow_connection) {
            return;
        }
        const bool already_connected = connect_mode_ == ConnectMode::HandlingConnectionRequest;
        their_internal_system_address_ = accepted->internal_addresses;
        onConnectedPong(accepted->send_ping_time, now);
        my_external_system_address_ = accepted->client_address;
        connect_mode_ = ConnectMode::Connected;
        reliability_layer.setMaximumHeldBytes(configuration_.maximum_held_bytes);

        auto buffer = Unpooled::buffer(MAXIMUM_MTU_SIZE, MAXIMUM_MTU_SIZE);
        NewIncomingConnection{.server_address = system_address_,
                              .send_ping_time = accepted->send_pong_time,
                              .send_pong_time = getTime(now)}
            .encode(buffer);
        sendImmediate(buffer.slice(), PacketPriority::Immediate, PacketReliability::ReliableOrdered, now);
        if (!already_connected) {
            pingInternal(PacketReliability::Unreliable, now);
        }
        announce();
        return;
    }
    default:
        return;
    }
}

void Connection::parseConnectionRequestPacket(ByteBufView message, TimePoint now)
{
    const auto request = ConnectionRequest::decode(message);
    if (!request || request->password != configuration_.password) {
        auto buffer = Unpooled::buffer(9, 9);
        InvalidPassword{configuration_.guid}.encode(buffer);
        sendImmediate(buffer.slice(0, 2), PacketPriority::Immediate, PacketReliability::Reliable, now);
        silent_disconnect_reason_ = DisconnectReason::Rejected;
        connect_mode_ = ConnectMode::DisconnectAsapSilently;
        return;
    }
    connect_mode_ = ConnectMode::HandlingConnectionRequest;
    onConnectionRequest(request->send_ping_time, now);
}

void Connection::onConnectionRequest(std::uint64_t incoming_timestamp, TimePoint now)
{
    auto buffer = Unpooled::buffer(MAXIMUM_MTU_SIZE, MAXIMUM_MTU_SIZE);
    ConnectionRequestAccepted{.client_address = system_address_,
                              .system_index = system_index_,
                              .send_ping_time = incoming_timestamp,
                              .send_pong_time = getTime(now)}
        .encode(buffer);
    sendImmediate(buffer.slice(), PacketPriority::Immediate, PacketReliability::ReliableOrdered, now);
}

void Connection::onConnectedPong(std::uint64_t send_ping_time, TimePoint now)
{
    const auto time = getTime(now);
    const auto ping = std::chrono::milliseconds{time > send_ping_time ? time - send_ping_time : 0};
    last_ping_ = ping;
    if (!lowest_ping_ || *lowest_ping_ > ping) {
        lowest_ping_ = ping;
    }
}

void Connection::pingInternal(PacketReliability reliability, TimePoint now)
{
    auto buffer = Unpooled::buffer(9, 9);
    ConnectedPing{getTime(now)}.encode(buffer);
    sendImmediate(buffer.slice(), PacketPriority::Immediate, reliability, now);
}

void Connection::sendImmediate(ByteBufView message, PacketPriority priority, PacketReliability reliability,
                               TimePoint now)
{
    if (!reliability_layer_ || connect_mode_ == ConnectMode::DisconnectAsap ||
        connect_mode_ == ConnectMode::DisconnectAsapSilently || connect_mode_ == ConnectMode::DisconnectOnNoAck ||
        connect_mode_ == ConnectMode::Closed) {
        return;
    }
    reliability_layer_->send(message.copy(), priority, reliability, 0, 0, now);
    if (reliability == PacketReliability::Reliable || reliability == PacketReliability::ReliableOrdered ||
        reliability == PacketReliability::ReliableSequenced ||
        reliability == PacketReliability::ReliableWithAckReceipt ||
        reliability == PacketReliability::ReliableOrderedWithAckReceipt) {
        last_reliable_send_ = now;
    }
}

bool Connection::send(ByteBuf message, PacketPriority priority, PacketReliability reliability,
                      std::uint8_t ordering_channel, TimePoint now)
{
    if (!reliability_layer_ || !message.isReadable() || connect_mode_ == ConnectMode::DisconnectAsap ||
        connect_mode_ == ConnectMode::DisconnectAsapSilently || connect_mode_ == ConnectMode::DisconnectOnNoAck ||
        connect_mode_ == ConnectMode::Closed) {
        return false;
    }
    reliability_layer_->send(std::move(message), priority, reliability, ordering_channel, 0, now);
    if (reliability == PacketReliability::Reliable || reliability == PacketReliability::ReliableOrdered ||
        reliability == PacketReliability::ReliableSequenced ||
        reliability == PacketReliability::ReliableWithAckReceipt ||
        reliability == PacketReliability::ReliableOrderedWithAckReceipt) {
        last_reliable_send_ = now;
    }
    if (!delivering_) {
        update(now);
    }
    return true;
}

void Connection::close(bool send_disconnection_notification, TimePoint now)
{
    if (isClosed()) {
        return;
    }
    const bool can_notify = reliability_layer_ && connect_mode_ != ConnectMode::DisconnectAsap &&
                            connect_mode_ != ConnectMode::DisconnectAsapSilently &&
                            connect_mode_ != ConnectMode::DisconnectOnNoAck;
    if (!send_disconnection_notification || !can_notify) {
        closeConnectionInternal(DisconnectReason::ClosedLocally);
        return;
    }
    constexpr std::array notification{static_cast<std::byte>(MessageId::DisconnectionNotification)};
    sendImmediate(Unpooled::wrappedBuffer(notification), PacketPriority::Low, PacketReliability::ReliableOrdered, now);
    connect_mode_ = ConnectMode::DisconnectAsap;
    if (!delivering_) {
        update(now);
    }
}

void Connection::sendOpenConnectionRequests(TimePoint now)
{
    const auto per_size =
        std::max(1U, configuration_.send_connection_attempt_count / static_cast<unsigned>(MTU_SIZES.size()));
    while (!isClosed() && now >= next_request_time_) {
        if (requests_made_ >= configuration_.send_connection_attempt_count + 1) {
            closeConnectionInternal(DisconnectReason::ConnectionAttemptFailed);
            return;
        }
        const auto mtu_size_index = std::min<std::size_t>(requests_made_ / per_size, MTU_SIZES.size() - 1);
        ++requests_made_;
        next_request_time_ = now + configuration_.time_between_send_connection_attempts;

        auto buffer = Unpooled::buffer(MAXIMUM_MTU_SIZE, MAXIMUM_MTU_SIZE);
        OpenConnectionRequest1{.protocol = RAKNET_PROTOCOL_VERSION,
                               .mtu = static_cast<std::uint16_t>(MTU_SIZES[mtu_size_index])}
            .encode(buffer);
        if (!listener_.sendMtuProbe(buffer.slice()) && mtu_size_index + 1 < MTU_SIZES.size()) {
            requests_made_ = static_cast<unsigned>((mtu_size_index + 1) * per_size);
            next_request_time_ = now;
        }
    }
}

void Connection::update(TimePoint now)
{
    if (isClosed()) {
        return;
    }
    if (connect_mode_ == ConnectMode::RequestingOpenConnection) {
        sendOpenConnectionRequests(now);
        return;
    }
    if (!reliability_layer_) {
        return;
    }
    auto &reliability_layer = *reliability_layer_;
    if (connect_mode_ == ConnectMode::Connected && now - last_reliable_send_ > configuration_.timeout_time / 2 &&
        reliability_layer.getMessagesInResendBuffer() == 0) {
        pingInternal(PacketReliability::Reliable, now);
        last_reliable_send_ = now;
    }
    if (connect_mode_ == ConnectMode::Connected && now >= next_ping_time_ && !lowest_ping_) {
        next_ping_time_ = now + PING_INTERVAL;
        pingInternal(PacketReliability::Unreliable, now);
    }

    delivering_ = true;
    reliability_layer.update(now);
    delivering_ = false;
    if (isClosed()) {
        return;
    }

    const auto mode = connect_mode_;
    const bool handshaking = mode == ConnectMode::RequestedConnection ||
                             mode == ConnectMode::HandlingConnectionRequest || mode == ConnectMode::UnverifiedSender;
    const bool disconnecting = mode == ConnectMode::DisconnectAsap || mode == ConnectMode::DisconnectAsapSilently;
    const bool drained = disconnecting && !reliability_layer.isOutgoingDataWaiting();
    const bool acknowledged = mode == ConnectMode::DisconnectOnNoAck &&
                              (!reliability_layer.areAcksWaiting() || reliability_layer.ackTimeout(now));
    const bool abandoned = handshaking && now - connection_time_ > CONNECTION_REQUEST_TIMEOUT;
    if (!reliability_layer.isDeadConnection() && !drained && !acknowledged && !abandoned) {
        return;
    }
    switch (mode) {
    case ConnectMode::Connected:
        closeConnectionInternal(DisconnectReason::ConnectionLost);
        return;
    case ConnectMode::RequestedConnection:
        closeConnectionInternal(DisconnectReason::ConnectionAttemptFailed);
        return;
    case ConnectMode::DisconnectAsap:
        closeConnectionInternal(DisconnectReason::ClosedLocally);
        return;
    case ConnectMode::DisconnectOnNoAck:
        closeConnectionInternal(DisconnectReason::ClosedByPeer);
        return;
    case ConnectMode::DisconnectAsapSilently:
        closeConnectionInternal(silent_disconnect_reason_.value_or(DisconnectReason::ClosedLocally));
        return;
    default:
        closeConnectionInternal(DisconnectReason::HandshakeTimedOut);
        return;
    }
}

std::optional<TimePoint> Connection::nextTimeout() const
{
    if (isClosed()) {
        return std::nullopt;
    }
    if (connect_mode_ == ConnectMode::RequestingOpenConnection || !reliability_layer_) {
        return next_request_time_;
    }
    std::optional<TimePoint> earliest = reliability_layer_->nextTimeout();
    const auto consider = [&earliest](TimePoint candidate) {
        if (!earliest || candidate < *earliest) {
            earliest = candidate;
        }
    };
    if (connect_mode_ == ConnectMode::Connected) {
        if (reliability_layer_->getMessagesInResendBuffer() == 0) {
            consider(last_reliable_send_ + configuration_.timeout_time / 2 + std::chrono::milliseconds{1});
        }
        if (!lowest_ping_) {
            consider(next_ping_time_);
        }
    }
    if (connect_mode_ == ConnectMode::RequestedConnection || connect_mode_ == ConnectMode::HandlingConnectionRequest ||
        connect_mode_ == ConnectMode::UnverifiedSender) {
        consider(connection_time_ + CONNECTION_REQUEST_TIMEOUT + std::chrono::milliseconds{1});
    }
    return earliest;
}

void Connection::announce()
{
    if (announced_) {
        return;
    }
    announced_ = true;
    listener_.onConnected();
}

void Connection::closeConnectionInternal(DisconnectReason reason)
{
    if (isClosed()) {
        return;
    }
    connect_mode_ = ConnectMode::Closed;
    listener_.onDisconnected(reason);
}

}  // namespace endgate::raknet
