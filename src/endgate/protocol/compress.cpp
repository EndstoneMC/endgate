#include "endgate/protocol/compress.h"

#include <libdeflate.h>
#include <snappy.h>

#include <algorithm>
#include <format>

namespace endgate {

void MinecraftCompressDecoder::setCompressionEnabled(bool enabled, PacketCompressionAlgorithm algorithm)
{
    if (enabled && !decompressor_) {
        decompressor_ = {libdeflate_alloc_decompressor(), libdeflate_free_decompressor};
    }
    enabled_ = enabled;
    algorithm_ = algorithm;
}

void MinecraftCompressDecoder::setHybridCompressionEnabled(bool enabled)
{
    hybrid_ = enabled;
}

ByteBuf MinecraftCompressDecoder::decompress(const ByteBufView &batch) const
{
    constexpr std::size_t max_decompressed_size = 64 * 1024 * 1024;

    const auto input = batch.nioBuffer();
    if (input.empty()) {
        throw DecoderException("batch is empty");
    }

    const CompressionHeader header =
        hybrid_ ? CompressionHeader{std::to_integer<std::uint8_t>(input.front())} : CompressionHeader{algorithm_};
    const auto payload = input.subspan(hybrid_ ? CompressionHeader::SIZE : 0);

    if (header.isCompressedWith(PacketCompressionAlgorithm::None)) {
        return Unpooled::copiedBuffer(payload);
    }

    if (!header.isCompressedWith(algorithm_)) {
        throw DecoderException("batch names an unknown compression algorithm");
    }

    if (header.isCompressedWith(PacketCompressionAlgorithm::Snappy)) {
        std::size_t size = 0;
        if (!snappy::GetUncompressedLength(reinterpret_cast<const char *>(payload.data()), payload.size(), &size)) {
            throw DecoderException("batch is corrupt");
        }
        if (size > max_decompressed_size) {
            throw DecoderException("batch expands beyond the permitted size");
        }

        auto out = Unpooled::buffer(size);
        if (!snappy::RawUncompress(reinterpret_cast<const char *>(payload.data()), payload.size(),
                                   reinterpret_cast<char *>(out.nioBuffer(0, size).data()))) {
            throw DecoderException("batch is corrupt");
        }
        out.writerIndex(size);
        return out;
    }

    if (header.isCompressedWith(PacketCompressionAlgorithm::ZLib)) {
        for (std::size_t capacity = std::max<std::size_t>(payload.size() * 6, 1024); capacity <= max_decompressed_size;
             capacity *= 2) {
            auto out = Unpooled::buffer(capacity);
            std::size_t produced = 0;
            const auto result = libdeflate_deflate_decompress(decompressor_.get(), payload.data(), payload.size(),
                                                              out.nioBuffer(0, capacity).data(), capacity, &produced);
            if (result == LIBDEFLATE_SUCCESS) {
                out.writerIndex(produced);
                return out;
            }
            if (result != LIBDEFLATE_INSUFFICIENT_SPACE) {
                throw DecoderException("batch is corrupt");
            }
        }
        throw DecoderException("batch expands beyond the permitted size");
    }

    throw DecoderException("batch names an unknown compression algorithm");
}

MinecraftCompressEncoder::MinecraftCompressEncoder(int compression_level)
    : compression_level_(compression_level < 0 ? DEFAULT_COMPRESSION_LEVEL : compression_level)
{
}

void MinecraftCompressEncoder::setCompressionEnabled(bool enabled, PacketCompressionAlgorithm algorithm,
                                                     std::uint16_t threshold)
{
    if (enabled && !compressor_) {
        compressor_ = {libdeflate_alloc_compressor(compression_level_), libdeflate_free_compressor};
    }
    enabled_ = enabled;
    algorithm_ = algorithm;
    threshold_ = threshold;
}

void MinecraftCompressEncoder::setHybridCompressionEnabled(bool enabled)
{
    hybrid_ = enabled;
}

void MinecraftCompressEncoder::compress(const ByteBufView &batch, ByteBuf &out) const
{
    const auto input = batch.nioBuffer();
    const std::size_t prefix = hybrid_ ? CompressionHeader::SIZE : 0;

    const bool worth_compressing = !hybrid_ || (threshold_ > 0 && input.size() >= threshold_);
    if (worth_compressing) {
        const auto bound = algorithm_ == PacketCompressionAlgorithm::Snappy
                             ? snappy::MaxCompressedLength(input.size())
                             : libdeflate_deflate_compress_bound(compressor_.get(), input.size());

        out.ensureWritable(bound + prefix);
        auto writable = out.nioBuffer(out.writerIndex(), bound + prefix);
        std::size_t produced = 0;

        if (algorithm_ == PacketCompressionAlgorithm::Snappy) {
            snappy::RawCompress(reinterpret_cast<const char *>(input.data()), input.size(),
                                reinterpret_cast<char *>(writable.data() + prefix), &produced);
        }
        else {
            produced = libdeflate_deflate_compress(compressor_.get(), input.data(), input.size(),
                                                   writable.data() + prefix, writable.size() - prefix);
        }

        if (produced > 0 && (produced < input.size() || !hybrid_)) {
            if (hybrid_) {
                writable[0] = static_cast<std::byte>(CompressionHeader{algorithm_}.getRaw());
            }
            out.writerIndex(out.writerIndex() + produced + prefix);
            return;
        }

        if (!hybrid_) {
            throw EncoderException("batch could not be compressed");
        }
    }

    if (hybrid_) {
        out.writeByte(CompressionHeader{PacketCompressionAlgorithm::None}.getRaw());
    }
    out.writeBytes(input);
}

}  // namespace endgate
