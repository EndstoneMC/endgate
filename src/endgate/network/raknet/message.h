#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/raknet/codec.h"
#include "endgate/network/raknet/protocol.h"

namespace endgate::raknet {

/**
 * @see RakNet::ProcessOfflineNetworkPacket
 */
[[nodiscard]] bool isOfflineMessage(const ByteBufView &datagram);

/**
 * @see ID_UNCONNECTED_PING
 */
struct UnconnectedPing {
    MessageId id{MessageId::UnconnectedPing};
    std::uint64_t send_ping_time{};
    std::uint64_t client_guid{UNASSIGNED_RAKNET_GUID};

    [[nodiscard]] static std::optional<UnconnectedPing> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_UNCONNECTED_PONG
 */
struct UnconnectedPong {
    std::uint64_t send_ping_time{};
    std::uint64_t server_guid{};
    ByteBufView data;

    [[nodiscard]] static std::optional<UnconnectedPong> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_OPEN_CONNECTION_REQUEST_1
 */
struct OpenConnectionRequest1 {
    std::uint8_t protocol{RAKNET_PROTOCOL_VERSION};
    std::uint16_t mtu{};

    [[nodiscard]] static std::optional<OpenConnectionRequest1> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_OPEN_CONNECTION_REPLY_1
 */
struct OpenConnectionReply1 {
    std::uint64_t server_guid{};
    std::optional<std::uint32_t> cookie;
    std::uint16_t mtu{};

    [[nodiscard]] static std::optional<OpenConnectionReply1> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_OPEN_CONNECTION_REQUEST_2
 */
struct OpenConnectionRequest2 {
    std::optional<std::uint32_t> cookie;
    SystemAddress server_address;
    std::uint16_t mtu{};
    std::uint64_t client_guid{UNASSIGNED_RAKNET_GUID};

    [[nodiscard]] static std::optional<OpenConnectionRequest2> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_OPEN_CONNECTION_REPLY_2
 */
struct OpenConnectionReply2 {
    std::uint64_t server_guid{};
    SystemAddress client_address;
    std::uint16_t mtu{};
    bool security{false};

    [[nodiscard]] static std::optional<OpenConnectionReply2> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_INCOMPATIBLE_PROTOCOL_VERSION
 */
struct IncompatibleProtocolVersion {
    std::uint8_t protocol{RAKNET_PROTOCOL_VERSION};
    std::uint64_t server_guid{};

    [[nodiscard]] static std::optional<IncompatibleProtocolVersion> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_ALREADY_CONNECTED
 * @see ID_NO_FREE_INCOMING_CONNECTIONS
 * @see ID_CONNECTION_BANNED
 * @see ID_IP_RECENTLY_CONNECTED
 * @see ID_CONNECTION_ATTEMPT_FAILED
 */
struct OfflineRejection {
    MessageId id{MessageId::AlreadyConnected};
    std::uint64_t server_guid{};

    [[nodiscard]] static std::optional<OfflineRejection> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_CONNECTED_PING
 */
struct ConnectedPing {
    std::uint64_t send_ping_time{};

    [[nodiscard]] static std::optional<ConnectedPing> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_CONNECTED_PONG
 */
struct ConnectedPong {
    std::uint64_t send_ping_time{};
    std::uint64_t send_pong_time{};

    [[nodiscard]] static std::optional<ConnectedPong> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_CONNECTION_REQUEST
 */
struct ConnectionRequest {
    std::uint64_t client_guid{};
    std::uint64_t send_ping_time{};
    bool security{false};
    ByteBufView password;

    [[nodiscard]] static std::optional<ConnectionRequest> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

using InternalAddresses = std::array<SystemAddress, MAXIMUM_NUMBER_OF_INTERNAL_IDS>;

/**
 * @see ID_CONNECTION_REQUEST_ACCEPTED
 */
struct ConnectionRequestAccepted {
    SystemAddress client_address;
    std::uint16_t system_index{};
    InternalAddresses internal_addresses{};
    std::uint64_t send_ping_time{};
    std::uint64_t send_pong_time{};

    [[nodiscard]] static std::optional<ConnectionRequestAccepted> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_NEW_INCOMING_CONNECTION
 */
struct NewIncomingConnection {
    SystemAddress server_address;
    InternalAddresses internal_addresses{};
    std::uint64_t send_ping_time{};
    std::uint64_t send_pong_time{};

    [[nodiscard]] static std::optional<NewIncomingConnection> decode(ByteBufView message);
    void encode(ByteBuf &writer) const;
};

/**
 * @see ID_INVALID_PASSWORD
 */
struct InvalidPassword {
    std::uint64_t server_guid{};

    void encode(ByteBuf &writer) const;
};

}  // namespace endgate::raknet
