#include "endgate/network/nethernet/framing.h"

#include <algorithm>
#include <array>
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

bool PacketQueue::push(const ByteBuf &packet, ESendType type)
{
    const auto size = packet.length();
    if (size <= fragment_size_) {
        sendFragment(0, packet.retainedSlice(0, size), type);
        return true;
    }
    if (size > MAX_FRAGMENTS * fragment_size_) {
        return false;
    }
    if (type != ESendType::Reliable) {
        return false;
    }

    auto fragments = static_cast<std::uint8_t>((size - 1) / fragment_size_ + 1);
    std::size_t offset = 0;
    while (fragments > 0) {
        --fragments;
        const auto length = std::min(size - offset, fragment_size_);
        sendFragment(fragments, packet.retainedSlice(offset, length), ESendType::Reliable);
        offset += length;
    }
    return true;
}

void PacketQueue::sendFragment(std::uint8_t header, ByteBuf payload, ESendType type)
{
    const std::array header_byte{static_cast<std::byte>(header)};
    auto fragment = ByteBuf::copyBuffer(header_byte);
    fragment.appendToChain(std::move(payload));
    listener_.sendFragment(type, std::move(fragment));
}

void PacketQueue::onOrderedMessage(const ByteBuf &message)
{
    if (message.length() <= 1) {
        partial_receive_.reset();
        listener_.closeChannel(ESendType::Reliable);
        return;
    }

    std::array<std::byte, 1> header_byte{};
    message.copyTo(header_byte);
    const auto header = std::to_integer<std::uint8_t>(header_byte.front());

    if (!partial_receive_) {
        partial_receive_.emplace(header, ByteBuf{});
    }
    else if (header != partial_receive_->expected_header) {
        partial_receive_.reset();
        listener_.closeChannel(ESendType::Reliable);
        return;
    }

    if (partial_receive_->data.length() + message.length() - 1 > max_packet_size_) {
        partial_receive_.reset();
        listener_.closeChannel(ESendType::Reliable);
        return;
    }

    partial_receive_->data.appendToChain(message.retainedSlice(1, message.length() - 1));
    if (header != 0) {
        --partial_receive_->expected_header;
        return;
    }

    auto packet = std::move(partial_receive_->data);
    partial_receive_.reset();
    listener_.onPacket(std::move(packet));
}

void PacketQueue::onUnorderedMessage(const ByteBuf &message)
{
    if (message.length() <= 1) {
        listener_.closeChannel(ESendType::Unreliable);
        return;
    }
    listener_.onPacket(message.retainedSlice(1, message.length() - 1));
}

}  // namespace endgate::nethernet
