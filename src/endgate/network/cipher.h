#pragma once

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/codec.h"

namespace endgate {

/**
 * Decrypts what a connection receives once login turns encryption on. Until then it passes batches on.
 * Encryption itself is not implemented, so enabling it fails every read.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftCipherDecoder
 * @see EncryptedNetworkPeer
 */
class MinecraftCipherDecoder : public MessageToMessageDecoder {
public:
    void setEnabled(bool enabled) { enabled_ = enabled; }

    template <typename Context>
    void decode(Context &ctx, ByteBufView batch)
    {
        if (enabled_) {
            throw DecoderException("encryption is not supported yet");
        }
        ctx.fireChannelRead(batch);
    }

private:
    bool enabled_{false};
};

/**
 * Encrypts what a connection sends once login turns encryption on. Until then it passes batches on.
 * Encryption itself is not implemented, so enabling it fails every write.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftCipherEncoder
 * @see EncryptedNetworkPeer
 */
class MinecraftCipherEncoder : public ChannelOutboundHandlerAdapter {
public:
    void setEnabled(bool enabled) { enabled_ = enabled; }

    template <typename Context>
    void write(Context &ctx, ByteBuf batch)
    {
        if (enabled_) {
            throw EncoderException("encryption is not supported yet");
        }
        ctx.write(std::move(batch));
    }

private:
    bool enabled_{false};
};

}  // namespace endgate
