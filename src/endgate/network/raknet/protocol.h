#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace endgate::raknet {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

/**
 * @see RAKNET_PROTOCOL_VERSION
 */
inline constexpr std::uint8_t RAKNET_PROTOCOL_VERSION = 11;

/**
 * @see MAXIMUM_MTU_SIZE
 */
inline constexpr std::size_t MAXIMUM_MTU_SIZE = 1600;

/**
 * @see RakPeer::defaultMTUSize
 */
inline constexpr std::size_t MINIMUM_MTU_SIZE = 576;

/**
 * @see mtuSizes
 */
inline constexpr std::array<std::size_t, 3> MTU_SIZES{MAXIMUM_MTU_SIZE, 1200, MINIMUM_MTU_SIZE};

/**
 * @see UDP_HEADER_SIZE
 */
inline constexpr std::size_t UDP_HEADER_SIZE = 28;

/**
 * @see MAXIMUM_NUMBER_OF_INTERNAL_IDS
 */
inline constexpr std::size_t MAXIMUM_NUMBER_OF_INTERNAL_IDS = 20;

/**
 * @see NUMBER_OF_ORDERED_STREAMS
 */
inline constexpr std::size_t NUMBER_OF_ORDERED_STREAMS = 32;

/**
 * @see UNASSIGNED_RAKNET_GUID
 */
inline constexpr std::uint64_t UNASSIGNED_RAKNET_GUID = ~std::uint64_t{0};

/**
 * @see OFFLINE_MESSAGE_DATA_ID
 */
inline constexpr std::array<std::byte, 16> OFFLINE_MESSAGE_DATA_ID{
    std::byte{0x00}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x00}, std::byte{0xFE}, std::byte{0xFE},
    std::byte{0xFE}, std::byte{0xFE}, std::byte{0xFD}, std::byte{0xFD}, std::byte{0xFD}, std::byte{0xFD},
    std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};

/**
 * @see DefaultMessageIDTypes
 */
enum class MessageId : std::uint8_t {
    ConnectedPing = 0x00,
    UnconnectedPing = 0x01,
    UnconnectedPingOpenConnections = 0x02,
    ConnectedPong = 0x03,
    DetectLostConnections = 0x04,
    OpenConnectionRequest1 = 0x05,
    OpenConnectionReply1 = 0x06,
    OpenConnectionRequest2 = 0x07,
    OpenConnectionReply2 = 0x08,
    ConnectionRequest = 0x09,
    SndReceiptAcked = 0x0E,
    SndReceiptLoss = 0x0F,
    ConnectionRequestAccepted = 0x10,
    ConnectionAttemptFailed = 0x11,
    AlreadyConnected = 0x12,
    NewIncomingConnection = 0x13,
    NoFreeIncomingConnections = 0x14,
    DisconnectionNotification = 0x15,
    ConnectionLost = 0x16,
    ConnectionBanned = 0x17,
    InvalidPassword = 0x18,
    IncompatibleProtocolVersion = 0x19,
    IpRecentlyConnected = 0x1A,
    Timestamp = 0x1B,
    UnconnectedPong = 0x1C,
};

/**
 * @see PacketReliability
 */
enum class PacketReliability : std::uint8_t {
    Unreliable = 0,
    UnreliableSequenced = 1,
    Reliable = 2,
    ReliableOrdered = 3,
    ReliableSequenced = 4,
    UnreliableWithAckReceipt = 5,
    ReliableWithAckReceipt = 6,
    ReliableOrderedWithAckReceipt = 7,
};

/**
 * @see PacketPriority
 */
enum class PacketPriority : std::uint8_t {
    Immediate = 0,
    High = 1,
    Medium = 2,
    Low = 3,
};

inline constexpr std::size_t NUMBER_OF_PRIORITIES = 4;

}  // namespace endgate::raknet
