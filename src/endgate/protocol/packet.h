#pragma once

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <bedrock/protocol/serializer.hpp>
#include <bedrock/protocol/stream.hpp>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/codec.h"
#include "endgate/network/version.h"
#include "endgate/protocol/state.h"

namespace endgate {

template <typename Packet>
std::string encodePacket(int protocol, const Packet &packet)
{
    std::string body;
    const auto encoded = visitProtocolVersion(protocol, [&]<int Version>() {
        if constexpr (std::is_same_v<bp::packet_of_t<Version, Packet::Id>, Packet>) {
            bp::BinaryWriter writer{body};
            const auto written = bp::encode_<Version>(packet, writer);
            if (!written) {
                throw EncoderException(
                    std::format("{} failed to encode: {}", bp::struct_name<Packet>(), written.error()));
            }
            return true;
        }
        else {
            return false;
        }
    });
    if (!encoded) {
        throw EncoderException(std::format("{} is not a packet of protocol {}", bp::struct_name<Packet>(), protocol));
    }
    return body;
}

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
                    auto decoded = bp::decode_<Registry::VERSION, Packet>(reader);
                    if (!decoded) {
                        const auto &error = decoded.error();
                        if (error.reason == bp::Errc::trailing_bytes) {
                            throw DecoderException(
                                std::format("packet {} left {} bytes unread", id, error.length - error.offset));
                        }
                        throw DecoderException(std::format("packet {} failed to decode: {}", id, error));
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
        const auto body = encodePacket(version_.getProtocol(), packet);
        out.writeUnsignedVarInt(static_cast<std::uint32_t>(Packet::Id));
        out.writeBytes(std::as_bytes(std::span{body}));
    }

private:
    ProtocolVersion version_{MINIMUM_VERSION};
};

}  // namespace endgate
