#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <bedrock/protocol/network.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/codec.h"

struct libdeflate_compressor;
struct libdeflate_decompressor;

namespace endgate {

using PacketCompressionAlgorithm = bedrock::protocol::PacketCompressionAlgorithm;

/**
 * The byte in front of a batch naming how it was compressed. Only the algorithm's low byte reaches the
 * wire, so a batch is matched on the raw byte rather than on the enumerator.
 *
 * @see CompressionHeader
 */
class CompressionHeader {
public:
    static constexpr std::size_t SIZE = 1;

    explicit CompressionHeader(std::uint8_t raw) : raw_(raw) {}
    explicit CompressionHeader(PacketCompressionAlgorithm algorithm) : raw_(static_cast<std::uint8_t>(algorithm)) {}

    [[nodiscard]] std::uint8_t getRaw() const { return raw_; }
    [[nodiscard]] bool isCompressedWith(PacketCompressionAlgorithm algorithm) const
    {
        return raw_ == static_cast<std::uint8_t>(algorithm);
    }

private:
    std::uint8_t raw_;
};

/**
 * Decompresses each batch once the handshake turns compression on, and passes it on until then.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftCompressDecoder
 * @see CompressedNetworkPeer
 */
class MinecraftCompressDecoder : public MessageToMessageDecoder {
public:
    void setCompressionEnabled(bool enabled, PacketCompressionAlgorithm algorithm);
    void setHybridCompressionEnabled(bool enabled);

    template <typename Context>
    void decode(Context &ctx, ByteBufView batch)
    {
        if (!enabled_) {
            ctx.fireChannelRead(batch);
            return;
        }
        const auto decompressed = decompress(batch);
        ctx.fireChannelRead(decompressed.slice());
    }

private:
    [[nodiscard]] ByteBuf decompress(const ByteBufView &batch) const;

    bool enabled_{false};
    PacketCompressionAlgorithm algorithm_{PacketCompressionAlgorithm::None};
    bool hybrid_{true};
    std::shared_ptr<libdeflate_decompressor> decompressor_;
};

/**
 * Compresses each batch once the handshake turns compression on, and passes it on until then.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftCompressorAndLengthEncoder
 * @see CompressedNetworkPeer
 */
class MinecraftCompressEncoder : public MessageToByteEncoder {
public:
    static constexpr int DEFAULT_COMPRESSION_LEVEL = 6;

    explicit MinecraftCompressEncoder(int compression_level = -1);

    void setCompressionEnabled(bool enabled, PacketCompressionAlgorithm algorithm, std::uint16_t threshold);
    void setHybridCompressionEnabled(bool enabled);

    template <typename Context>
    void write(Context &ctx, ByteBuf batch)
    {
        if (!enabled_) {
            ctx.write(std::move(batch));
            return;
        }
        MessageToByteEncoder::write(ctx, std::move(batch));
    }

    template <typename Context>
    void encode(Context &, const ByteBuf &batch, ByteBuf &out)
    {
        compress(batch.slice(), out);
    }

private:
    void compress(const ByteBufView &batch, ByteBuf &out) const;

    int compression_level_;
    bool enabled_{false};
    PacketCompressionAlgorithm algorithm_{PacketCompressionAlgorithm::None};
    std::uint16_t threshold_{0};
    bool hybrid_{true};
    std::shared_ptr<libdeflate_compressor> compressor_;
};

}  // namespace endgate
