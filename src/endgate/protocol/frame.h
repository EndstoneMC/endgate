#pragma once

#include <format>
#include <utility>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/codec.h"

namespace endgate {

/**
 * Splits a batch into its packets at each packet's unsigned varint length once login turns framing on,
 * and passes the batch on whole until then. A length that is zero or runs past the batch is corrupt.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftVarintFrameDecoder
 */
class MinecraftVarintFrameDecoder : public ByteToMessageDecoder {
public:
    void setEnabled(bool enabled) { enabled_ = enabled; }

    template <typename Context>
    void decode(Context &ctx, ByteBufView &in)
    {
        if (!enabled_) {
            ctx.fireChannelRead(in.readSlice(in.readableBytes()));
            return;
        }
        const auto length = in.readUnsignedVarInt();
        if (length == 0) {
            throw CorruptedFrameException("packet length is zero");
        }
        if (length > in.readableBytes()) {
            throw CorruptedFrameException(
                std::format("packet length {} exceeds the {} bytes left in the batch", length, in.readableBytes()));
        }
        ctx.fireChannelRead(in.readSlice(length));
    }

private:
    bool enabled_{false};
};

/**
 * Appends each packet to the batch behind its unsigned varint length once login turns framing on, and
 * hands the batch on at flush. Until then it passes each write on as the batch it is.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftVarintLengthEncoder
 */
class MinecraftVarintLengthEncoder : public ChannelOutboundHandlerAdapter {
public:
    void setEnabled(bool enabled) { enabled_ = enabled; }

    template <typename Context>
    void write(Context &ctx, ByteBuf packet)
    {
        if (!enabled_) {
            ctx.write(std::move(packet));
            return;
        }
        batch_.writeUnsignedVarInt(static_cast<std::uint32_t>(packet.readableBytes()));
        batch_.writeBytes(packet.nioBuffer());
    }

    template <typename Context>
    void flush(Context &ctx)
    {
        if (batch_.isReadable()) {
            ctx.write(std::exchange(batch_, ByteBuf{}));
        }
        ctx.flush();
    }

private:
    bool enabled_{false};
    ByteBuf batch_;
};

}  // namespace endgate
