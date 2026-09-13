#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "endgate/network/buffer.h"

namespace endgate {

/**
 * Writes to the tail of a chain, growing it when the tail runs out. The primitives past push are the
 * ones Bedrock encodes its own structures with.
 *
 * @see folly::io::Appender
 * @see BinaryStream
 */
class Appender {
public:
    Appender(ByteBuf &buf, std::size_t growth) : buf_(buf), growth_(growth) {}

    /**
     * @see folly::io::Appender::ensure
     */
    void ensure(std::size_t count)
    {
        if (buf_.tailroom() < count) {
            buf_.appendToChain(ByteBuf::create(std::max(count, growth_)));
        }
    }

    /**
     * @see folly::io::Appender::push
     */
    void push(std::span<const std::byte> bytes)
    {
        ensure(bytes.size());
        std::copy(bytes.begin(), bytes.end(), buf_.writableTail().begin());
        buf_.append(bytes.size());
    }

    void writeByte(std::uint8_t value) { push({reinterpret_cast<const std::byte *>(&value), sizeof(value)}); }

    void writeBool(bool value) { writeByte(value ? 1 : 0); }

    /**
     * @see folly::io::Appender::writeLE
     */
    void writeSignedIntLE(std::int32_t value)
    {
        const auto bits = static_cast<std::uint32_t>(value);
        for (auto shift = 0U; shift < 32U; shift += 8U) {
            writeByte(static_cast<std::uint8_t>(bits >> shift));
        }
    }

    /**
     * @see BinaryStream::writeUnsignedVarInt
     */
    void writeUnsignedVarInt(std::uint32_t value)
    {
        do {
            const auto byte = static_cast<std::uint8_t>(value & 0x7FU);
            value >>= 7U;
            writeByte(value ? byte | 0x80U : byte);
        } while (value);
    }

    /**
     * @see BinaryStream::writeVarInt
     */
    void writeVarInt(std::int32_t value) { writeUnsignedVarInt((value >> 31) ^ (value << 1)); }

    /**
     * @see BinaryStream::writeString
     */
    void writeString(std::string_view value)
    {
        writeUnsignedVarInt(static_cast<std::uint32_t>(value.size()));
        push({reinterpret_cast<const std::byte *>(value.data()), value.size()});
    }

private:
    ByteBuf &buf_;
    std::size_t growth_;
};

}  // namespace endgate
