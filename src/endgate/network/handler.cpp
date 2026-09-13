#include "endgate/network/handler.h"

#include <libdeflate.h>
#include <snappy.h>

#include <algorithm>
#include <expected>
#include <utility>
#include <vector>

namespace endgate {

namespace {
std::span<const std::byte> gather(const ByteBuf &batch, std::vector<std::byte> &scratch)
{
    if (const auto contiguous = batch.coalesce(); !contiguous.empty()) {
        return contiguous;
    }
    scratch.resize(batch.length());
    batch.copyTo(scratch);
    return scratch;
}
}  // namespace

CompressionHandler::CompressionHandler(int compression_level)
{
    constexpr int default_compression_level = 6;
    compressor_ = {libdeflate_alloc_compressor(compression_level < 0 ? default_compression_level : compression_level),
                   libdeflate_free_compressor};
    decompressor_ = {libdeflate_alloc_decompressor(), libdeflate_free_decompressor};
}

void CompressionHandler::setCompressionEnabled(bool enabled, PacketCompressionAlgorithm algorithm,
                                               std::uint16_t threshold)
{
    enabled_ = enabled;
    algorithm_ = algorithm;
    threshold_ = threshold;
}

void CompressionHandler::setHybridCompressionEnabled(bool enabled)
{
    hybrid_ = enabled;
}

std::expected<ByteBuf, DecodeError> CompressionHandler::decompress(const ByteBuf &batch)
{
    constexpr std::size_t max_decompressed_size = 64 * 1024 * 1024;

    std::vector<std::byte> scratch;
    const auto input = gather(batch, scratch);
    if (input.empty()) {
        return std::unexpected(DecodeError::EmptyBatch);
    }

    const CompressionHeader header =
        hybrid_ ? CompressionHeader{std::to_integer<std::uint8_t>(input.front())} : CompressionHeader{algorithm_};
    const auto payload = input.subspan(hybrid_ ? CompressionHeader::SIZE : 0);

    if (header.isCompressedWith(PacketCompressionAlgorithm::None)) {
        return ByteBuf::copyBuffer(payload);
    }

    if (!header.isCompressedWith(algorithm_)) {
        return std::unexpected(DecodeError::UnknownCompressionAlgorithm);
    }

    if (header.isCompressedWith(PacketCompressionAlgorithm::Snappy)) {
        std::size_t size = 0;
        if (!snappy::GetUncompressedLength(reinterpret_cast<const char *>(payload.data()), payload.size(), &size)) {
            return std::unexpected(DecodeError::CorruptCompressedData);
        }
        if (size > max_decompressed_size) {
            return std::unexpected(DecodeError::DecompressedSizeExceeded);
        }

        auto out = ByteBuf::create(size);
        if (!snappy::RawUncompress(reinterpret_cast<const char *>(payload.data()), payload.size(),
                                   reinterpret_cast<char *>(out.writableTail().data()))) {
            return std::unexpected(DecodeError::CorruptCompressedData);
        }
        out.append(size);
        return out;
    }

    if (header.isCompressedWith(PacketCompressionAlgorithm::ZLib)) {
        for (std::size_t capacity = std::max<std::size_t>(payload.size() * 6, 1024); capacity <= max_decompressed_size;
             capacity *= 2) {
            auto out = ByteBuf::create(capacity);
            std::size_t produced = 0;
            const auto result = libdeflate_deflate_decompress(decompressor_.get(), payload.data(), payload.size(),
                                                              out.writableTail().data(), capacity, &produced);
            if (result == LIBDEFLATE_SUCCESS) {
                out.append(produced);
                return out;
            }
            if (result != LIBDEFLATE_INSUFFICIENT_SPACE) {
                return std::unexpected(DecodeError::CorruptCompressedData);
            }
        }
        return std::unexpected(DecodeError::DecompressedSizeExceeded);
    }

    return std::unexpected(DecodeError::UnknownCompressionAlgorithm);
}

std::expected<ByteBuf, DecodeError> CompressionHandler::compress(const ByteBuf &batch)
{
    std::vector<std::byte> scratch;
    const auto input = gather(batch, scratch);
    const std::size_t prefix = hybrid_ ? CompressionHeader::SIZE : 0;

    const bool worth_compressing = !hybrid_ || (threshold_ > 0 && input.size() >= threshold_);
    if (worth_compressing) {
        const auto bound = algorithm_ == PacketCompressionAlgorithm::Snappy
                             ? snappy::MaxCompressedLength(input.size())
                             : libdeflate_deflate_compress_bound(compressor_.get(), input.size());

        auto out = ByteBuf::create(bound + prefix);
        auto writable = out.writableTail();
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
            out.append(produced + prefix);
            return out;
        }

        if (!hybrid_) {
            return std::unexpected(DecodeError::CompressionFailed);
        }
    }

    auto out = ByteBuf::create(input.size() + prefix);
    auto writable = out.writableTail();
    if (hybrid_) {
        writable[0] = static_cast<std::byte>(CompressionHeader{PacketCompressionAlgorithm::None}.getRaw());
    }
    std::copy(input.begin(), input.end(), writable.begin() + prefix);
    out.append(input.size() + prefix);
    return out;
}

}  // namespace endgate
