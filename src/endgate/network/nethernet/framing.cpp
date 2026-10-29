#include "endgate/network/nethernet/framing.h"

#include <algorithm>
#include <utility>

namespace endgate::nethernet {

PacketQueue::PacketQueue(Listener &listener, std::size_t max_packet_size)
    : listener_(listener), max_packet_size_(max_packet_size)
{
}

bool PacketQueue::setMaxMessageSize(std::size_t max_message_size)
{
    if (max_message_size <= 1) {
        return false;
    }
    fragment_size_ = max_message_size - 1;
    return true;
}

bool PacketQueue::push(ByteBufView packet, ESendType type)
{
    const auto size = packet.readableBytes();
    if (size > fragment_size_ && (size > MAX_FRAGMENTS * fragment_size_ || type != ESendType::Reliable)) {
        return false;
    }

    auto fragments = static_cast<std::uint8_t>(size <= fragment_size_ ? 1 : (size - 1) / fragment_size_ + 1);
    std::size_t offset = 0;
    while (fragments > 0) {
        --fragments;
        const auto length = std::min(size - offset, fragment_size_);
        auto fragment = Unpooled::buffer(1 + length);
        fragment.writeByte(fragments).writeBytes(packet.nioBuffer(packet.readerIndex() + offset, length));
        listener_.sendFragment(type, std::move(fragment));
        offset += length;
    }
    return true;
}

void PacketQueue::onOrderedMessage(ByteBufView message)
{
    if (message.readableBytes() <= 1) {
        partial_receive_.reset();
        listener_.closeChannel(ESendType::Reliable);
        return;
    }

    const auto header = message.readUnsignedByte();
    if (!partial_receive_ && header == 0 && message.readableBytes() <= max_packet_size_) {
        listener_.onPacket(message);
        return;
    }

    if (!partial_receive_) {
        partial_receive_.emplace(header, ByteBuf{});
    }
    else if (header != partial_receive_->expected_header) {
        partial_receive_.reset();
        listener_.closeChannel(ESendType::Reliable);
        return;
    }

    if (partial_receive_->data.readableBytes() + message.readableBytes() > max_packet_size_) {
        partial_receive_.reset();
        listener_.closeChannel(ESendType::Reliable);
        return;
    }

    partial_receive_->data.writeBytes(message.nioBuffer());
    if (header != 0) {
        --partial_receive_->expected_header;
        return;
    }

    auto packet = std::move(partial_receive_->data);
    partial_receive_.reset();
    listener_.onPacket(packet.slice());
}

void PacketQueue::onUnorderedMessage(ByteBufView message)
{
    if (message.readableBytes() <= 1) {
        listener_.closeChannel(ESendType::Unreliable);
        return;
    }
    message.skipBytes(1);
    listener_.onPacket(message);
}

}  // namespace endgate::nethernet
