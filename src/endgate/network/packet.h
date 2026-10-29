#pragma once

#include <concepts>
#include <cstdint>
#include <format>
#include <utility>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/codec.h"

namespace endgate {

/**
 * A packet the codec can decode and encode: it names its id, owns its fields, reads them from a
 * buffer past the header and writes them back.
 *
 * @see com.velocitypowered.proxy.protocol.MinecraftPacket
 */
template <typename Packet>
concept MinecraftPacket = requires(ByteBufView &in, ByteBuf &out, const Packet &packet) {
    { Packet::ID } -> std::convertible_to<std::uint32_t>;
    { Packet::decode(in) } -> std::same_as<Packet>;
    packet.encode(out);
};

/**
 * Decodes the packets registered with it once login turns decoding on, and hands every other packet,
 * and every packet until then, to the next stage as the bytes it arrived in. A registered packet that
 * leaves bytes unread is corrupt.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftDecoder
 * @see PacketHeader
 */
template <MinecraftPacket... Packets>
class MinecraftDecoder : public MessageToMessageDecoder {
public:
    static constexpr std::uint32_t PACKET_ID_MASK = 0x3FF;

    void setEnabled(bool enabled) { enabled_ = enabled; }

    template <typename Context>
    void decode(Context &ctx, ByteBufView packet)
    {
        if (!enabled_) {
            ctx.fireChannelRead(packet);
            return;
        }
        const auto start = packet.readerIndex();
        const auto id = packet.readUnsignedVarInt() & PACKET_ID_MASK;
        if (!(decodeRegistered<Packets>(ctx, packet, id) || ...)) {
            packet.readerIndex(start);
            ctx.fireChannelRead(packet);
        }
    }

private:
    template <typename Packet, typename Context>
    static bool decodeRegistered(Context &ctx, ByteBufView &packet, std::uint32_t id)
    {
        if (id != Packet::ID) {
            return false;
        }
        auto decoded = Packet::decode(packet);
        if (packet.isReadable()) {
            throw DecoderException(std::format("packet {} left {} bytes unread", id, packet.readableBytes()));
        }
        ctx.fireChannelRead(std::move(decoded));
        return true;
    }

    bool enabled_{false};
};

/**
 * Encodes the packets registered with it, behind their header, and passes a packet already encoded, a
 * ByteBuf, on as it is.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftEncoder
 * @see PacketHeader
 */
template <MinecraftPacket... Packets>
class MinecraftEncoder : public MessageToByteEncoder {
public:
    template <typename Context, typename Packet>
        requires(std::same_as<Packet, Packets> || ...)
    void encode(Context &, const Packet &packet, ByteBuf &out)
    {
        out.writeUnsignedVarInt(Packet::ID);
        packet.encode(out);
    }
};

}  // namespace endgate
