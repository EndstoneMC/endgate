#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "endgate/network/buffer.h"
#include "endgate/network/raknet/message.h"
#include "endgate/network/raknet/protocol.h"
#include "endgate/network/raknet/reliability.h"

namespace endgate::raknet {

enum class DisconnectReason {
    ConnectionLost,
    ClosedByPeer,
    ClosedLocally,
    ConnectionAttemptFailed,
    InvalidPassword,
    AlreadyConnected,
    NoFreeIncomingConnections,
    ConnectionBanned,
    IncompatibleProtocolVersion,
    IpRecentlyConnected,
    HandshakeTimedOut,
    Rejected,
};

[[nodiscard]] constexpr std::string_view toString(DisconnectReason reason)
{
    switch (reason) {
    case DisconnectReason::ConnectionLost:
        return "connection lost";
    case DisconnectReason::ClosedByPeer:
        return "closed by peer";
    case DisconnectReason::ClosedLocally:
        return "closed locally";
    case DisconnectReason::ConnectionAttemptFailed:
        return "connection attempt failed";
    case DisconnectReason::InvalidPassword:
        return "invalid password";
    case DisconnectReason::AlreadyConnected:
        return "already connected";
    case DisconnectReason::NoFreeIncomingConnections:
        return "no free incoming connections";
    case DisconnectReason::ConnectionBanned:
        return "connection banned";
    case DisconnectReason::IncompatibleProtocolVersion:
        return "incompatible protocol version";
    case DisconnectReason::IpRecentlyConnected:
        return "ip recently connected";
    case DisconnectReason::HandshakeTimedOut:
        return "handshake timed out";
    case DisconnectReason::Rejected:
        return "rejected";
    }
    return "unknown";
}

/**
 * @see RakNet::RakPeer
 */
struct ConnectionConfiguration {
    std::uint64_t guid{UNASSIGNED_RAKNET_GUID};
    std::chrono::milliseconds timeout_time{10000};
    std::chrono::milliseconds unreliable_timeout{1000};
    std::vector<std::byte> password;
    unsigned send_connection_attempt_count{12};
    std::chrono::milliseconds time_between_send_connection_attempts{500};
    std::size_t maximum_held_bytes{ReliabilityLayer::MAXIMUM_HELD_BYTES};
};

/**
 * @see RakNet::RakPeer::RemoteSystemStruct
 * @see RakNet::RakPeer::RequestedConnectionStruct
 */
class Connection : private ReliabilityLayer::Listener {
public:
    class Listener {
    public:
        virtual void sendDatagram(std::span<const std::byte> datagram) = 0;
        virtual bool sendMtuProbe(std::span<const std::byte> datagram) = 0;
        virtual void onConnected() = 0;
        virtual void onMessage(ByteBuf message) = 0;
        virtual void onDisconnected(DisconnectReason reason) = 0;
        virtual void onReliabilityLayerNotification(std::string_view message, bool is_error) = 0;

    protected:
        ~Listener() = default;
    };

    /**
     * @see RakNet::RakPeer::RemoteSystemStruct::ConnectMode
     */
    enum class ConnectMode {
        RequestingOpenConnection,
        DisconnectAsap,
        DisconnectAsapSilently,
        DisconnectOnNoAck,
        RequestedConnection,
        HandlingConnectionRequest,
        UnverifiedSender,
        Connected,
        Closed,
    };

    Connection(Listener &listener, ConnectionConfiguration configuration, SystemAddress system_address,
               const OpenConnectionRequest2 &request, std::uint16_t system_index, TimePoint now);
    Connection(Listener &listener, ConnectionConfiguration configuration, SystemAddress system_address, TimePoint now);
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&) = delete;
    Connection &operator=(Connection &&) = delete;
    ~Connection() = default;

    void handleDatagram(std::span<const std::byte> datagram, TimePoint now);
    void handleTimeout(TimePoint now);
    [[nodiscard]] std::optional<TimePoint> nextTimeout() const;

    /**
     * @see RakNet::RakPeer::Send
     */
    bool send(ByteBuf message, PacketPriority priority, PacketReliability reliability, std::uint8_t ordering_channel,
              TimePoint now);

    /**
     * @see RakNet::RakPeer::CloseConnection
     */
    void close(bool send_disconnection_notification, TimePoint now);

    [[nodiscard]] const SystemAddress &getSystemAddress() const { return system_address_; }
    [[nodiscard]] std::uint64_t getGuid() const { return guid_; }
    [[nodiscard]] std::uint16_t getSystemIndex() const { return system_index_; }
    [[nodiscard]] ConnectMode getConnectMode() const { return connect_mode_; }
    [[nodiscard]] bool isConnected() const { return connect_mode_ == ConnectMode::Connected; }
    [[nodiscard]] bool isClosed() const { return connect_mode_ == ConnectMode::Closed; }
    [[nodiscard]] bool weInitiatedTheConnection() const { return we_initiated_the_connection_; }
    [[nodiscard]] std::size_t getMtuSize() const { return mtu_size_; }
    [[nodiscard]] std::optional<std::chrono::milliseconds> getLastPing() const { return last_ping_; }
    [[nodiscard]] const SystemAddress &getExternalId() const { return my_external_system_address_; }

private:
    void sendDatagram(std::span<const std::byte> datagram) override;
    void onReliabilityLayerNotification(std::string_view message, bool is_error) override;

    void handleOfflineMessage(std::span<const std::byte> datagram, TimePoint now);
    void handleMessages(TimePoint now);
    void handleMessage(ByteBuf message, TimePoint now);
    void parseConnectionRequestPacket(std::span<const std::byte> message, TimePoint now);
    void onConnectionRequest(std::uint64_t incoming_timestamp, TimePoint now);
    void onConnectedPong(std::uint64_t send_ping_time, TimePoint now);
    void pingInternal(PacketReliability reliability, TimePoint now);
    void sendImmediate(std::span<const std::byte> message, PacketPriority priority, PacketReliability reliability,
                       TimePoint now);
    void sendOpenConnectionRequests(TimePoint now);
    void update(TimePoint now);
    void announce();
    void closeConnectionInternal(DisconnectReason reason);
    [[nodiscard]] static std::uint64_t getTime(TimePoint now);

    Listener &listener_;
    ConnectionConfiguration configuration_;
    SystemAddress system_address_;
    SystemAddress my_external_system_address_;
    InternalAddresses their_internal_system_address_{};
    std::uint64_t guid_{UNASSIGNED_RAKNET_GUID};
    std::uint16_t system_index_{0};
    bool we_initiated_the_connection_;
    bool announced_{false};
    bool delivering_{false};
    ConnectMode connect_mode_;
    std::optional<DisconnectReason> silent_disconnect_reason_;
    std::size_t mtu_size_{MINIMUM_MTU_SIZE};
    std::optional<ReliabilityLayer> reliability_layer_;
    TimePoint connection_time_;
    TimePoint last_reliable_send_;
    TimePoint next_ping_time_;
    std::optional<std::chrono::milliseconds> last_ping_;
    std::optional<std::chrono::milliseconds> lowest_ping_;
    unsigned requests_made_{0};
    TimePoint next_request_time_;
};

}  // namespace endgate::raknet
