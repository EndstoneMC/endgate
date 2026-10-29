#pragma once

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <bedrock/protocol/serializer.hpp>
#include <bedrock/protocol/stream.hpp>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/codec.h"
#include "endgate/network/version.h"
#include "endgate/protocol/state.h"

namespace endgate {

template <typename Packet>
struct DecodedPacket {
    Packet packet;
    ByteBufView bytes;
};

/**
 * Decodes the packets the connection's state registers in its direction at its protocol version once
 * login turns decoding on, and hands every other packet, and every packet until then, to the next stage
 * as the bytes it arrived in. A registered packet that fails to decode or leaves bytes unread is
 * corrupt.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftDecoder
 * @see PacketHeader
 */
class MinecraftDecoder : public MessageToMessageDecoder {
public:
    static constexpr std::uint32_t PACKET_ID_MASK = 0x3FF;

    explicit MinecraftDecoder(Direction direction) : direction_(direction) {}

    void setEnabled(bool enabled) { enabled_ = enabled; }

    /**
     * @see com.velocitypowered.proxy.protocol.netty.MinecraftDecoder#setProtocolVersion
     */
    void setProtocolVersion(ProtocolVersion version) { version_ = version; }

    /**
     * @see com.velocitypowered.proxy.protocol.netty.MinecraftDecoder#setState
     */
    void setState(StateRegistry state) { state_ = state; }

    /**
     * @see com.velocitypowered.proxy.protocol.netty.MinecraftDecoder#getDirection
     */
    [[nodiscard]] Direction getDirection() const { return direction_; }

    template <typename Context>
    void decode(Context &ctx, ByteBufView packet)
    {
        if (!enabled_) {
            ctx.fireChannelRead(packet);
            return;
        }
        const auto whole = packet;
        const auto id = static_cast<int>(packet.readUnsignedVarInt() & PACKET_ID_MASK);
        const auto body = packet.nioBuffer();
        const auto registered =
            getProtocolRegistry(state_, direction_, version_.getProtocol(), [&]<typename Registry>() {
                return Registry::createPacket(id, [&]<typename Packet>() {
                    bp::BinaryReader reader{std::string_view{reinterpret_cast<const char *>(body.data()), body.size()}};
                    auto decoded = bp::deserialize<Packet>(reader);
                    if (!decoded) {
                        throw DecoderException(
                            std::format("packet {} failed to decode: {}", id, decoded.error().message()));
                    }
                    if (reader.getUnreadLength() != 0) {
                        throw DecoderException(
                            std::format("packet {} left {} bytes unread", id, reader.getUnreadLength()));
                    }
                    ctx.fireChannelRead(DecodedPacket<Packet>{.packet = std::move(*decoded), .bytes = whole});
                });
            });
        if (!registered) {
            ctx.fireChannelRead(whole);
        }
    }

private:
    Direction direction_;
    StateRegistry state_{StateRegistry::Login};
    ProtocolVersion version_{MINIMUM_VERSION};
    bool enabled_{false};
};

/**
 * Encodes a packet of the connection's protocol version behind its header, and passes a packet already
 * encoded, a ByteBuf, on as it is. A packet of another version is refused.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftEncoder
 * @see PacketHeader
 */
class MinecraftEncoder : public MessageToByteEncoder {
public:
    /**
     * @see com.velocitypowered.proxy.protocol.netty.MinecraftEncoder#setProtocolVersion
     */
    void setProtocolVersion(ProtocolVersion version) { version_ = version; }

    template <typename Context, typename Packet>
        requires requires { Packet::Id; }
    void encode(Context &, const Packet &packet, ByteBuf &out)
    {
        if (!isPacketOf<Packet>(version_.getProtocol())) {
            throw EncoderException(
                std::format("{} is not a packet of protocol {}", bp::struct_name<Packet>(), version_.getProtocol()));
        }
        std::string body;
        bp::BinaryWriter writer{body};
        bp::serialize(writer, packet);
        out.writeUnsignedVarInt(static_cast<std::uint32_t>(Packet::Id)).writeBytes(std::as_bytes(std::span{body}));
    }

private:
    ProtocolVersion version_{MINIMUM_VERSION};
};

}  // namespace endgate
