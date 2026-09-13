#include "endgate/network/handler.h"

#include <libdeflate.h>
#include <snappy.h>

#include <algorithm>
#include <expected>
#include <utility>
#include <vector>

namespace endgate {

namespace {
constexpr int default_compression_level = 6;
constexpr std::size_t max_decompressed_size = 8 * 1024 * 1024;

std::span<const std::byte> gather(const ByteBuf &batch, std::vector<std::byte> &scratch)
{
    if (const auto contiguous = batch.contiguous(); !contiguous.empty()) {
        return contiguous;
    }
    scratch.resize(batch.readableBytes());
    batch.copyTo(scratch);
    return scratch;
}
}  // namespace

CompressionHandler::CompressionHandler(int compression_level)
    : compressor_(libdeflate_alloc_compressor(compression_level < 0 ? default_compression_level : compression_level),
                  libdeflate_free_compressor),
      decompressor_(libdeflate_alloc_decompressor(), libdeflate_free_decompressor)
{
}

CompressionHandler::~CompressionHandler() = default;
CompressionHandler::CompressionHandler(CompressionHandler &&other) noexcept = default;
CompressionHandler &CompressionHandler::operator=(CompressionHandler &&other) noexcept = default;

void CompressionHandler::setCompressionEnabled(bool enabled, PacketCompressionAlgorithm algorithm,
                                               std::uint16_t threshold)
{
    enabled_ = enabled;
    algorithm_ = algorithm;
    threshold_ = threshold;
}

std::expected<ByteBuf, DecodeError> CompressionHandler::decompress(const ByteBuf &batch)
{
    std::vector<std::byte> scratch;
    const auto input = gather(batch, scratch);
    if (input.empty()) {
        return std::unexpected(DecodeError::EmptyBatch);
    }

    const CompressionHeader header{std::to_integer<std::uint8_t>(input.front())};
    const auto payload = input.subspan(CompressionHeader::SIZE);

    if (header.isCompressedWith(PacketCompressionAlgorithm::None)) {
        return ByteBuf::copyOf(payload);
    }

    if (header.isCompressedWith(PacketCompressionAlgorithm::Snappy)) {
        std::size_t size = 0;
        if (!snappy::GetUncompressedLength(reinterpret_cast<const char *>(payload.data()), payload.size(), &size)) {
            return std::unexpected(DecodeError::CorruptCompressedData);
        }
        if (size > max_decompressed_size) {
            return std::unexpected(DecodeError::DecompressedSizeExceeded);
        }

        auto out = ByteBuf::allocate(size);
        if (!snappy::RawUncompress(reinterpret_cast<const char *>(payload.data()), payload.size(),
                                   reinterpret_cast<char *>(out.writable().data()))) {
            return std::unexpected(DecodeError::CorruptCompressedData);
        }
        out.advanceWriter(size);
        return out;
    }

    if (header.isCompressedWith(PacketCompressionAlgorithm::ZLib)) {
        for (std::size_t capacity = std::max<std::size_t>(payload.size() * 4, 1024); capacity <= max_decompressed_size;
             capacity *= 2) {
            auto out = ByteBuf::allocate(capacity);
            std::size_t produced = 0;
            const auto result = libdeflate_deflate_decompress(decompressor_.get(), payload.data(), payload.size(),
                                                              out.writable().data(), capacity, &produced);
            if (result == LIBDEFLATE_SUCCESS) {
                out.advanceWriter(produced);
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

    const auto algorithm = input.size() < threshold_ ? PacketCompressionAlgorithm::None : algorithm_;
    const auto header = static_cast<std::byte>(CompressionHeader{algorithm}.getRaw());

    if (algorithm == PacketCompressionAlgorithm::None) {
        auto out = ByteBuf::allocate(input.size() + CompressionHeader::SIZE);
        auto writable = out.writable();
        writable[0] = header;
        std::copy(input.begin(), input.end(), writable.begin() + CompressionHeader::SIZE);
        out.advanceWriter(input.size() + CompressionHeader::SIZE);
        return out;
    }

    if (algorithm == PacketCompressionAlgorithm::Snappy) {
        auto out = ByteBuf::allocate(snappy::MaxCompressedLength(input.size()) + CompressionHeader::SIZE);
        auto writable = out.writable();
        writable[0] = header;

        std::size_t produced = 0;
        snappy::RawCompress(reinterpret_cast<const char *>(input.data()), input.size(),
                            reinterpret_cast<char *>(writable.data() + CompressionHeader::SIZE), &produced);
        out.advanceWriter(produced + CompressionHeader::SIZE);
        return out;
    }

    auto out =
        ByteBuf::allocate(libdeflate_deflate_compress_bound(compressor_.get(), input.size()) + CompressionHeader::SIZE);
    auto writable = out.writable();
    writable[0] = header;

    const auto produced = libdeflate_deflate_compress(compressor_.get(), input.data(), input.size(),
                                                      writable.data() + CompressionHeader::SIZE,
                                                      writable.size() - CompressionHeader::SIZE);
    if (produced == 0) {
        return std::unexpected(DecodeError::CompressionFailed);
    }
    out.advanceWriter(produced + CompressionHeader::SIZE);
    return out;
}

}  // namespace endgate
