#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "endgate/network/buffer.h"

namespace endgate::nethernet {

/**
 * @see NetherNet::ESendType
 */
enum class ESendType : std::uint8_t {
    Unreliable = 0,
    Reliable = 1,
};

/**
 * Frames packets onto NetherNet's two data channels. Every message carries one header byte counting
 * the messages still to follow, so a reliable packet larger than one message goes out as a countdown
 * of fragments and is joined back when the count reaches zero.
 *
 * @see NetherNet::PacketQueue
 */
class PacketQueue {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void sendFragment(ESendType type, ByteBuf fragment) = 0;
        virtual void onPacket(ByteBuf packet) = 0;
        virtual void closeChannel(ESendType type) = 0;
    };

    static constexpr std::size_t DEFAULT_MAX_MESSAGE_SIZE = 262144;
    static constexpr std::size_t MAX_FRAGMENTS = 255;

    PacketQueue(Listener &listener, std::size_t max_packet_size);

    bool setMaxMessageSize(std::size_t max_message_size);

    bool push(const ByteBuf &packet, ESendType type);

    void onOrderedMessage(const ByteBuf &message);

    void onUnorderedMessage(const ByteBuf &message);

private:
    struct PartialReceive {
        std::uint8_t expected_header;
        ByteBuf data;
    };

    void sendFragment(std::uint8_t header, ByteBuf payload, ESendType type);

    Listener &listener_;
    std::size_t max_packet_size_;
    std::size_t fragment_size_{DEFAULT_MAX_MESSAGE_SIZE - 1};
    std::optional<PartialReceive> partial_receive_;
};

}  // namespace endgate::nethernet
