#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "endgate/network/buffer.h"

namespace endgate {

enum class PacketCompressionAlgorithm : std::uint16_t {
    ZLib = 0,
    Snappy = 1,
    None = 65535,
};

/**
 * Splits one received batch into the packets it carries, and joins packets back into a batch on the
 * way out. fireRead is called once per packet, so a batch fans out without anything polling for it.
 *
 * @see BatchedNetworkPeer
 */
class BatchHandler {
public:
    template <typename Context>
    void read(Context &context, ByteBuf batch)
    {
        std::size_t offset = 0;
        while (offset < batch.readableBytes()) {
            const auto length = batch.readableBytes() - offset;
            context.fireRead(batch.retainedSlice(offset, length));
            offset += length;
        }
    }

    template <typename Context>
    void write(Context &context, ByteBuf packet)
    {
        context.write(std::move(packet));
    }
};

/**
 * Compresses and decompresses a batch. Stays a pass-through until the handshake turns compression on,
 * the way BDS gates it on a flag rather than by restructuring the chain.
 *
 * @see CompressedNetworkPeer
 */
class CompressionHandler {
public:
    void setCompressionEnabled(bool enabled, PacketCompressionAlgorithm algorithm)
    {
        enabled_ = enabled;
        algorithm_ = algorithm;
    }

    template <typename Context>
    void read(Context &context, ByteBuf batch)
    {
        if (!enabled_) {
            context.fireRead(std::move(batch));
            return;
        }
        context.fireRead(std::move(batch));
    }

    template <typename Context>
    void write(Context &context, ByteBuf batch)
    {
        context.write(std::move(batch));
    }

private:
    bool enabled_{false};
    PacketCompressionAlgorithm algorithm_{PacketCompressionAlgorithm::None};
};

/**
 * Encrypts and decrypts a batch. Stays a pass-through until login completes.
 *
 * @see EncryptedNetworkPeer
 */
class EncryptionHandler {
public:
    void setEncryptionEnabled(bool enabled) { enabled_ = enabled; }

    template <typename Context>
    void read(Context &context, ByteBuf batch)
    {
        if (!enabled_) {
            context.fireRead(std::move(batch));
            return;
        }
        context.fireRead(std::move(batch));
    }

    template <typename Context>
    void write(Context &context, ByteBuf batch)
    {
        context.write(std::move(batch));
    }

private:
    bool enabled_{false};
};

}  // namespace endgate
