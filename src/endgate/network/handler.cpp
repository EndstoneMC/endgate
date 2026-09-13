#include "endgate/network/handler.h"

#include <libdeflate.h>
#include <snappy.h>

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

namespace endgate {

namespace {
constexpr int compression_level = 6;
constexpr std::uint8_t zlib_header = 0x00;
constexpr std::uint8_t snappy_header = 0x01;
constexpr std::uint8_t none_header = 0xFF;
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

CompressionHandler::CompressionHandler()
    : compressor_(libdeflate_alloc_compressor(compression_level), libdeflate_free_compressor),
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

ByteBuf CompressionHandler::decompress(const ByteBuf &batch)
{
    std::vector<std::byte> scratch;
    const auto input = gather(batch, scratch);
    if (input.empty()) {
        throw std::runtime_error("compressed batch is empty");
    }

    const auto header = std::to_integer<std::uint8_t>(input.front());
    const auto payload = input.subspan(1);

    switch (header) {
    case none_header:
        return ByteBuf::copyOf(payload);

    case snappy_header: {
        std::size_t size = 0;
        if (!snappy::GetUncompressedLength(reinterpret_cast<const char *>(payload.data()), payload.size(), &size)) {
            throw std::runtime_error("snappy batch has no length prefix");
        }
        if (size > max_decompressed_size) {
            throw std::runtime_error(std::format("snappy batch expands to {} bytes", size));
        }

        auto out = ByteBuf::allocate(size);
        if (!snappy::RawUncompress(reinterpret_cast<const char *>(payload.data()), payload.size(),
                                   reinterpret_cast<char *>(out.writable().data()))) {
            throw std::runtime_error("snappy batch is corrupt");
        }
        out.advanceWriter(size);
        return out;
    }

    case zlib_header: {
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
                throw std::runtime_error("deflate batch is corrupt");
            }
        }
        throw std::runtime_error("deflate batch expands beyond the permitted size");
    }

    default:
        break;
    }

    throw std::runtime_error(std::format("unknown compression algorithm {}", header));
}

ByteBuf CompressionHandler::compress(const ByteBuf &batch)
{
    std::vector<std::byte> scratch;
    const auto input = gather(batch, scratch);

    const auto algorithm = input.size() < threshold_ ? PacketCompressionAlgorithm::None : algorithm_;
    const auto header = static_cast<std::byte>(algorithm == PacketCompressionAlgorithm::Snappy ? snappy_header
                                               : algorithm == PacketCompressionAlgorithm::ZLib ? zlib_header
                                                                                               : none_header);

    if (algorithm == PacketCompressionAlgorithm::None) {
        auto out = ByteBuf::allocate(input.size() + 1);
        auto writable = out.writable();
        writable[0] = header;
        std::copy(input.begin(), input.end(), writable.begin() + 1);
        out.advanceWriter(input.size() + 1);
        return out;
    }

    if (algorithm == PacketCompressionAlgorithm::Snappy) {
        auto out = ByteBuf::allocate(snappy::MaxCompressedLength(input.size()) + 1);
        auto writable = out.writable();
        writable[0] = header;

        std::size_t produced = 0;
        snappy::RawCompress(reinterpret_cast<const char *>(input.data()), input.size(),
                            reinterpret_cast<char *>(writable.data() + 1), &produced);
        out.advanceWriter(produced + 1);
        return out;
    }

    auto out = ByteBuf::allocate(libdeflate_deflate_compress_bound(compressor_.get(), input.size()) + 1);
    auto writable = out.writable();
    writable[0] = header;

    const auto produced = libdeflate_deflate_compress(compressor_.get(), input.data(), input.size(),
                                                      writable.data() + 1, writable.size() - 1);
    if (produced == 0) {
        throw std::runtime_error("deflate produced no output");
    }
    out.advanceWriter(produced + 1);
    return out;
}

}  // namespace endgate
