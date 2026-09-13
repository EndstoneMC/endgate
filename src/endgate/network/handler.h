#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <utility>

#include "endgate/network/buffer.h"

struct libdeflate_compressor;
struct libdeflate_decompressor;

namespace endgate {

/**
 * Why a batch could not be turned back into packets.
 *
 * @see NetworkPeer::DataStatus::BrokenData
 */
enum class DecodeError {
    EmptyBatch,
    UnknownCompressionAlgorithm,
    CorruptCompressedData,
    DecompressedSizeExceeded,
    CompressionFailed,
};

[[nodiscard]] constexpr std::string_view toString(DecodeError error)
{
    switch (error) {
    case DecodeError::EmptyBatch:
        return "batch is empty";
    case DecodeError::UnknownCompressionAlgorithm:
        return "batch names an unknown compression algorithm";
    case DecodeError::CorruptCompressedData:
        return "batch is corrupt";
    case DecodeError::DecompressedSizeExceeded:
        return "batch expands beyond the permitted size";
    case DecodeError::CompressionFailed:
        return "batch could not be compressed";
    }
    return "unknown";
}

enum class PacketCompressionAlgorithm : std::uint16_t {
    ZLib = 0,
    Snappy = 1,
    None = 65535,
};

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
    explicit CompressionHandler(int compression_level);
    ~CompressionHandler();
    CompressionHandler(CompressionHandler &&other) noexcept;
    CompressionHandler &operator=(CompressionHandler &&other) noexcept;

    void setCompressionEnabled(bool enabled, PacketCompressionAlgorithm algorithm, std::uint16_t threshold);

    template <typename Context>
    void read(Context &context, ByteBuf batch)
    {
        if (!enabled_) {
            context.fireRead(std::move(batch));
            return;
        }
        auto decompressed = decompress(batch);
        if (!decompressed) {
            context.fireExceptionCaught(decompressed.error());
            return;
        }
        context.fireRead(*std::move(decompressed));
    }

    template <typename Context>
    void write(Context &context, ByteBuf batch)
    {
        if (!enabled_) {
            context.write(std::move(batch));
            return;
        }
        auto compressed = compress(batch);
        if (!compressed) {
            context.fireExceptionCaught(compressed.error());
            return;
        }
        context.write(*std::move(compressed));
    }

private:
    [[nodiscard]] std::expected<ByteBuf, DecodeError> decompress(const ByteBuf &batch);
    [[nodiscard]] std::expected<ByteBuf, DecodeError> compress(const ByteBuf &batch);

    bool enabled_{false};
    PacketCompressionAlgorithm algorithm_{PacketCompressionAlgorithm::None};
    std::uint16_t threshold_{0};
    std::unique_ptr<libdeflate_compressor, void (*)(libdeflate_compressor *)> compressor_;
    std::unique_ptr<libdeflate_decompressor, void (*)(libdeflate_decompressor *)> decompressor_;
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
