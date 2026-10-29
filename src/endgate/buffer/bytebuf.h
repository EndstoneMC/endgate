#pragma once

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace endgate {

class ByteBuf;
class ByteBufView;

/**
 * Holds the reader and writer indices and the read surface that ByteBuf and ByteBufView share.
 *
 * @see io.netty.buffer.AbstractByteBuf
 */
class AbstractByteBuf {
public:
    static constexpr std::size_t DEFAULT_MAX_CAPACITY = std::numeric_limits<std::int32_t>::max();

    [[nodiscard]] std::size_t readerIndex() const { return reader_index_; }
    [[nodiscard]] std::size_t writerIndex() const { return writer_index_; }
    [[nodiscard]] std::size_t readableBytes() const { return writer_index_ - reader_index_; }
    [[nodiscard]] bool isReadable() const { return writer_index_ > reader_index_; }
    [[nodiscard]] bool isReadable(std::size_t size) const { return writer_index_ - reader_index_ >= size; }

    template <typename Self>
    Self &readerIndex(this Self &self, std::size_t reader_index)
    {
        if (reader_index > self.writer_index_) {
            throw std::out_of_range(std::format("readerIndex: {} (expected: 0 <= readerIndex <= writerIndex({}))",
                                                reader_index, self.writer_index_));
        }
        self.reader_index_ = reader_index;
        return self;
    }

    template <typename Self>
    Self &markReaderIndex(this Self &self)
    {
        self.marked_reader_index_ = self.reader_index_;
        return self;
    }

    template <typename Self>
    Self &resetReaderIndex(this Self &self)
    {
        return self.readerIndex(self.marked_reader_index_);
    }

    template <typename Self>
    Self &skipBytes(this Self &self, std::size_t length)
    {
        self.checkReadableBytes(length);
        self.reader_index_ += length;
        return self;
    }

    template <typename Self>
    [[nodiscard]] bool getBoolean(this const Self &self, std::size_t index)
    {
        return self.getByte(index) != 0;
    }

    template <typename Self>
    [[nodiscard]] std::int8_t getByte(this const Self &self, std::size_t index)
    {
        return std::bit_cast<std::int8_t>(self.getUnsignedByte(index));
    }

    template <typename Self>
    [[nodiscard]] std::uint8_t getUnsignedByte(this const Self &self, std::size_t index)
    {
        return self.template getValue<std::uint8_t, std::endian::big>(index);
    }

    template <typename Self>
    [[nodiscard]] std::int16_t getShort(this const Self &self, std::size_t index)
    {
        return std::bit_cast<std::int16_t>(self.getUnsignedShort(index));
    }

    template <typename Self>
    [[nodiscard]] std::int16_t getShortLE(this const Self &self, std::size_t index)
    {
        return std::bit_cast<std::int16_t>(self.getUnsignedShortLE(index));
    }

    template <typename Self>
    [[nodiscard]] std::uint16_t getUnsignedShort(this const Self &self, std::size_t index)
    {
        return self.template getValue<std::uint16_t, std::endian::big>(index);
    }

    template <typename Self>
    [[nodiscard]] std::uint16_t getUnsignedShortLE(this const Self &self, std::size_t index)
    {
        return self.template getValue<std::uint16_t, std::endian::little>(index);
    }

    template <typename Self>
    [[nodiscard]] std::int32_t getMedium(this const Self &self, std::size_t index)
    {
        const auto value = self.getUnsignedMedium(index);
        return std::bit_cast<std::int32_t>((value & 0x800000U) != 0 ? value | 0xFF000000U : value);
    }

    template <typename Self>
    [[nodiscard]] std::int32_t getMediumLE(this const Self &self, std::size_t index)
    {
        const auto value = self.getUnsignedMediumLE(index);
        return std::bit_cast<std::int32_t>((value & 0x800000U) != 0 ? value | 0xFF000000U : value);
    }

    template <typename Self>
    [[nodiscard]] std::uint32_t getUnsignedMedium(this const Self &self, std::size_t index)
    {
        self.checkIndex(index, 3);
        const auto *bytes = self.memory() + index;
        return std::to_integer<std::uint32_t>(bytes[0]) << 16U | std::to_integer<std::uint32_t>(bytes[1]) << 8U |
               std::to_integer<std::uint32_t>(bytes[2]);
    }

    template <typename Self>
    [[nodiscard]] std::uint32_t getUnsignedMediumLE(this const Self &self, std::size_t index)
    {
        self.checkIndex(index, 3);
        const auto *bytes = self.memory() + index;
        return std::to_integer<std::uint32_t>(bytes[0]) | std::to_integer<std::uint32_t>(bytes[1]) << 8U |
               std::to_integer<std::uint32_t>(bytes[2]) << 16U;
    }

    template <typename Self>
    [[nodiscard]] std::int32_t getInt(this const Self &self, std::size_t index)
    {
        return std::bit_cast<std::int32_t>(self.getUnsignedInt(index));
    }

    template <typename Self>
    [[nodiscard]] std::int32_t getIntLE(this const Self &self, std::size_t index)
    {
        return std::bit_cast<std::int32_t>(self.getUnsignedIntLE(index));
    }

    template <typename Self>
    [[nodiscard]] std::uint32_t getUnsignedInt(this const Self &self, std::size_t index)
    {
        return self.template getValue<std::uint32_t, std::endian::big>(index);
    }

    template <typename Self>
    [[nodiscard]] std::uint32_t getUnsignedIntLE(this const Self &self, std::size_t index)
    {
        return self.template getValue<std::uint32_t, std::endian::little>(index);
    }

    template <typename Self>
    [[nodiscard]] std::int64_t getLong(this const Self &self, std::size_t index)
    {
        return std::bit_cast<std::int64_t>(self.template getValue<std::uint64_t, std::endian::big>(index));
    }

    template <typename Self>
    [[nodiscard]] std::int64_t getLongLE(this const Self &self, std::size_t index)
    {
        return std::bit_cast<std::int64_t>(self.template getValue<std::uint64_t, std::endian::little>(index));
    }

    template <typename Self>
    [[nodiscard]] float getFloat(this const Self &self, std::size_t index)
    {
        return std::bit_cast<float>(self.getUnsignedInt(index));
    }

    template <typename Self>
    [[nodiscard]] float getFloatLE(this const Self &self, std::size_t index)
    {
        return std::bit_cast<float>(self.getUnsignedIntLE(index));
    }

    template <typename Self>
    [[nodiscard]] double getDouble(this const Self &self, std::size_t index)
    {
        return std::bit_cast<double>(self.template getValue<std::uint64_t, std::endian::big>(index));
    }

    template <typename Self>
    [[nodiscard]] double getDoubleLE(this const Self &self, std::size_t index)
    {
        return std::bit_cast<double>(self.template getValue<std::uint64_t, std::endian::little>(index));
    }

    template <typename Self>
    void getBytes(this const Self &self, std::size_t index, std::span<std::byte> dst)
    {
        self.checkIndex(index, dst.size());
        if (!dst.empty()) {
            std::memcpy(dst.data(), self.memory() + index, dst.size());
        }
    }

    template <typename Self>
    [[nodiscard]] bool readBoolean(this Self &self)
    {
        return self.readByte() != 0;
    }

    template <typename Self>
    [[nodiscard]] std::int8_t readByte(this Self &self)
    {
        return std::bit_cast<std::int8_t>(self.readUnsignedByte());
    }

    template <typename Self>
    [[nodiscard]] std::uint8_t readUnsignedByte(this Self &self)
    {
        return self.getUnsignedByte(self.template advanceReader<1>());
    }

    template <typename Self>
    [[nodiscard]] std::int16_t readShort(this Self &self)
    {
        return self.getShort(self.template advanceReader<2>());
    }

    template <typename Self>
    [[nodiscard]] std::int16_t readShortLE(this Self &self)
    {
        return self.getShortLE(self.template advanceReader<2>());
    }

    template <typename Self>
    [[nodiscard]] std::uint16_t readUnsignedShort(this Self &self)
    {
        return self.getUnsignedShort(self.template advanceReader<2>());
    }

    template <typename Self>
    [[nodiscard]] std::uint16_t readUnsignedShortLE(this Self &self)
    {
        return self.getUnsignedShortLE(self.template advanceReader<2>());
    }

    template <typename Self>
    [[nodiscard]] std::int32_t readMedium(this Self &self)
    {
        return self.getMedium(self.template advanceReader<3>());
    }

    template <typename Self>
    [[nodiscard]] std::int32_t readMediumLE(this Self &self)
    {
        return self.getMediumLE(self.template advanceReader<3>());
    }

    template <typename Self>
    [[nodiscard]] std::uint32_t readUnsignedMedium(this Self &self)
    {
        return self.getUnsignedMedium(self.template advanceReader<3>());
    }

    template <typename Self>
    [[nodiscard]] std::uint32_t readUnsignedMediumLE(this Self &self)
    {
        return self.getUnsignedMediumLE(self.template advanceReader<3>());
    }

    template <typename Self>
    [[nodiscard]] std::int32_t readInt(this Self &self)
    {
        return self.getInt(self.template advanceReader<4>());
    }

    template <typename Self>
    [[nodiscard]] std::int32_t readIntLE(this Self &self)
    {
        return self.getIntLE(self.template advanceReader<4>());
    }

    template <typename Self>
    [[nodiscard]] std::uint32_t readUnsignedInt(this Self &self)
    {
        return self.getUnsignedInt(self.template advanceReader<4>());
    }

    template <typename Self>
    [[nodiscard]] std::uint32_t readUnsignedIntLE(this Self &self)
    {
        return self.getUnsignedIntLE(self.template advanceReader<4>());
    }

    template <typename Self>
    [[nodiscard]] std::int64_t readLong(this Self &self)
    {
        return self.getLong(self.template advanceReader<8>());
    }

    template <typename Self>
    [[nodiscard]] std::int64_t readLongLE(this Self &self)
    {
        return self.getLongLE(self.template advanceReader<8>());
    }

    template <typename Self>
    [[nodiscard]] float readFloat(this Self &self)
    {
        return self.getFloat(self.template advanceReader<4>());
    }

    template <typename Self>
    [[nodiscard]] float readFloatLE(this Self &self)
    {
        return self.getFloatLE(self.template advanceReader<4>());
    }

    template <typename Self>
    [[nodiscard]] double readDouble(this Self &self)
    {
        return self.getDouble(self.template advanceReader<8>());
    }

    template <typename Self>
    [[nodiscard]] double readDoubleLE(this Self &self)
    {
        return self.getDoubleLE(self.template advanceReader<8>());
    }

    template <typename Self>
    Self &readBytes(this Self &self, std::span<std::byte> dst)
    {
        self.checkReadableBytes(dst.size());
        self.getBytes(self.reader_index_, dst);
        self.reader_index_ += dst.size();
        return self;
    }

    template <typename Self>
    [[nodiscard]] std::uint32_t readUnsignedVarInt(this Self &self)
    {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 35; shift += 7) {
            const auto byte = self.readUnsignedByte();
            if (shift == 28 && byte > 0x0F) {
                throw std::out_of_range("varint is too long (expected: at most 5 bytes)");
            }
            value |= static_cast<std::uint32_t>(byte & 0x7FU) << shift;
            if ((byte & 0x80U) == 0) {
                return value;
            }
        }
        throw std::out_of_range("varint is too long (expected: at most 5 bytes)");
    }

    template <typename Self>
    [[nodiscard]] std::int32_t readVarInt(this Self &self)
    {
        const auto value = self.readUnsignedVarInt();
        return static_cast<std::int32_t>(value >> 1U) ^ -static_cast<std::int32_t>(value & 1U);
    }

    template <typename Self>
    [[nodiscard]] std::uint64_t readUnsignedVarInt64(this Self &self)
    {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 70; shift += 7) {
            const auto byte = self.readUnsignedByte();
            if (shift == 63 && byte > 0x01) {
                throw std::out_of_range("varint64 is too long (expected: at most 10 bytes)");
            }
            value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
            if ((byte & 0x80U) == 0) {
                return value;
            }
        }
        throw std::out_of_range("varint64 is too long (expected: at most 10 bytes)");
    }

    template <typename Self>
    [[nodiscard]] std::int64_t readVarInt64(this Self &self)
    {
        const auto value = self.readUnsignedVarInt64();
        return static_cast<std::int64_t>(value >> 1U) ^ -static_cast<std::int64_t>(value & 1U);
    }

    template <typename Self>
    [[nodiscard]] std::string readString(this Self &self, std::size_t max_length)
    {
        const auto length = self.readUnsignedVarInt();
        if (length > max_length) {
            throw std::out_of_range(std::format("string length: {} (expected: <= {})", length, max_length));
        }
        self.checkReadableBytes(length);
        std::string value(length, '\0');
        self.readBytes(std::as_writable_bytes(std::span{value}));
        return value;
    }

    template <typename Self>
    [[nodiscard]] ByteBufView readSlice(this Self &self, std::size_t length);

    template <typename Self>
    [[nodiscard]] ByteBufView slice(this const Self &self);

    template <typename Self>
    [[nodiscard]] ByteBufView slice(this const Self &self, std::size_t index, std::size_t length);

    template <typename Self>
    [[nodiscard]] ByteBufView duplicate(this const Self &self);

    template <typename Self>
    [[nodiscard]] ByteBuf copy(this const Self &self);

    template <typename Self>
    [[nodiscard]] ByteBuf copy(this const Self &self, std::size_t index, std::size_t length);

protected:
    AbstractByteBuf() = default;
    AbstractByteBuf(std::size_t reader_index, std::size_t writer_index)
        : reader_index_(reader_index), writer_index_(writer_index)
    {
    }

    template <typename Self>
    void checkIndex(this const Self &self, std::size_t index, std::size_t field_length)
    {
        if (index > self.capacity() || field_length > self.capacity() - index) {
            throw std::out_of_range(
                std::format("index: {}, length: {} (expected: range(0, {}))", index, field_length, self.capacity()));
        }
    }

    void checkReadableBytes(std::size_t minimum_readable_bytes) const
    {
        if (minimum_readable_bytes > readableBytes()) {
            throw std::out_of_range(std::format("readerIndex({}) + length({}) exceeds writerIndex({})", reader_index_,
                                                minimum_readable_bytes, writer_index_));
        }
    }

    template <typename T, std::endian Order, typename Self>
    [[nodiscard]] T getValue(this const Self &self, std::size_t index)
    {
        self.checkIndex(index, sizeof(T));
        T value{};
        std::memcpy(&value, self.memory() + index, sizeof(T));
        if constexpr (Order != std::endian::native) {
            value = std::byteswap(value);
        }
        return value;
    }

    template <std::size_t Size, typename Self>
    [[nodiscard]] std::size_t advanceReader(this Self &self)
    {
        self.checkReadableBytes(Size);
        const auto index = self.reader_index_;
        self.reader_index_ += Size;
        return index;
    }

    std::size_t reader_index_{0};
    std::size_t writer_index_{0};
    std::size_t marked_reader_index_{0};
};

/**
 * Bytes owned by the buffer, read from the reader index and appended at the writer index. Grows on demand up to its
 * maximum capacity, and never zero-fills what it allocates.
 *
 * @see io.netty.buffer.ByteBuf
 * @see io.netty.buffer.UnpooledHeapByteBuf
 */
class ByteBuf : public AbstractByteBuf {
public:
    ByteBuf() = default;
    ByteBuf(std::size_t initial_capacity, std::size_t max_capacity);
    ByteBuf(const ByteBuf &other) = delete;
    ByteBuf &operator=(const ByteBuf &other) = delete;
    ByteBuf(ByteBuf &&other) noexcept;
    ByteBuf &operator=(ByteBuf &&other) noexcept;
    ~ByteBuf() = default;

    [[nodiscard]] std::size_t capacity() const { return capacity_; }
    ByteBuf &capacity(std::size_t new_capacity);
    [[nodiscard]] std::size_t maxCapacity() const { return max_capacity_; }
    [[nodiscard]] std::size_t writableBytes() const { return capacity_ - writer_index_; }
    [[nodiscard]] std::size_t maxWritableBytes() const { return max_capacity_ - writer_index_; }
    [[nodiscard]] bool isWritable() const { return capacity_ > writer_index_; }
    [[nodiscard]] bool isWritable(std::size_t size) const { return capacity_ - writer_index_ >= size; }

    using AbstractByteBuf::writerIndex;
    ByteBuf &writerIndex(std::size_t writer_index);
    ByteBuf &setIndex(std::size_t reader_index, std::size_t writer_index);
    ByteBuf &markWriterIndex();
    ByteBuf &resetWriterIndex();
    ByteBuf &clear();
    ByteBuf &discardReadBytes();
    ByteBuf &ensureWritable(std::size_t min_writable_bytes);

    ByteBuf &setBoolean(std::size_t index, bool value) { return setByte(index, value ? 1 : 0); }
    ByteBuf &setByte(std::size_t index, std::uint8_t value)
    {
        return setValue<std::uint8_t, std::endian::big>(index, value);
    }
    ByteBuf &setShort(std::size_t index, std::uint16_t value)
    {
        return setValue<std::uint16_t, std::endian::big>(index, value);
    }
    ByteBuf &setShortLE(std::size_t index, std::uint16_t value)
    {
        return setValue<std::uint16_t, std::endian::little>(index, value);
    }
    ByteBuf &setMedium(std::size_t index, std::uint32_t value);
    ByteBuf &setMediumLE(std::size_t index, std::uint32_t value);
    ByteBuf &setInt(std::size_t index, std::uint32_t value)
    {
        return setValue<std::uint32_t, std::endian::big>(index, value);
    }
    ByteBuf &setIntLE(std::size_t index, std::uint32_t value)
    {
        return setValue<std::uint32_t, std::endian::little>(index, value);
    }
    ByteBuf &setLong(std::size_t index, std::uint64_t value)
    {
        return setValue<std::uint64_t, std::endian::big>(index, value);
    }
    ByteBuf &setLongLE(std::size_t index, std::uint64_t value)
    {
        return setValue<std::uint64_t, std::endian::little>(index, value);
    }
    ByteBuf &setFloat(std::size_t index, float value) { return setInt(index, std::bit_cast<std::uint32_t>(value)); }
    ByteBuf &setFloatLE(std::size_t index, float value) { return setIntLE(index, std::bit_cast<std::uint32_t>(value)); }
    ByteBuf &setDouble(std::size_t index, double value) { return setLong(index, std::bit_cast<std::uint64_t>(value)); }
    ByteBuf &setDoubleLE(std::size_t index, double value)
    {
        return setLongLE(index, std::bit_cast<std::uint64_t>(value));
    }
    ByteBuf &setBytes(std::size_t index, std::span<const std::byte> src);
    ByteBuf &setZero(std::size_t index, std::size_t length);

    ByteBuf &writeBoolean(bool value) { return writeByte(value ? 1 : 0); }
    ByteBuf &writeByte(std::uint8_t value) { return writeValue<std::uint8_t, std::endian::big>(value); }
    ByteBuf &writeShort(std::uint16_t value) { return writeValue<std::uint16_t, std::endian::big>(value); }
    ByteBuf &writeShortLE(std::uint16_t value) { return writeValue<std::uint16_t, std::endian::little>(value); }
    ByteBuf &writeMedium(std::uint32_t value);
    ByteBuf &writeMediumLE(std::uint32_t value);
    ByteBuf &writeInt(std::uint32_t value) { return writeValue<std::uint32_t, std::endian::big>(value); }
    ByteBuf &writeIntLE(std::uint32_t value) { return writeValue<std::uint32_t, std::endian::little>(value); }
    ByteBuf &writeLong(std::uint64_t value) { return writeValue<std::uint64_t, std::endian::big>(value); }
    ByteBuf &writeLongLE(std::uint64_t value) { return writeValue<std::uint64_t, std::endian::little>(value); }
    ByteBuf &writeFloat(float value) { return writeInt(std::bit_cast<std::uint32_t>(value)); }
    ByteBuf &writeFloatLE(float value) { return writeIntLE(std::bit_cast<std::uint32_t>(value)); }
    ByteBuf &writeDouble(double value) { return writeLong(std::bit_cast<std::uint64_t>(value)); }
    ByteBuf &writeDoubleLE(double value) { return writeLongLE(std::bit_cast<std::uint64_t>(value)); }
    ByteBuf &writeBytes(std::span<const std::byte> src);
    ByteBuf &writeBytes(ByteBufView &src);
    ByteBuf &writeBytes(const ByteBufView &src, std::size_t src_index, std::size_t length);
    ByteBuf &writeZero(std::size_t length);
    ByteBuf &writeUnsignedVarInt(std::uint32_t value) { return writeUnsignedVarInt64(value); }
    ByteBuf &writeVarInt(std::int32_t value)
    {
        return writeUnsignedVarInt(static_cast<std::uint32_t>(value) << 1U ^ static_cast<std::uint32_t>(value >> 31));
    }
    ByteBuf &writeUnsignedVarInt64(std::uint64_t value)
    {
        while (value >= 0x80U) {
            writeByte(static_cast<std::uint8_t>(value | 0x80U));
            value >>= 7U;
        }
        return writeByte(static_cast<std::uint8_t>(value));
    }
    ByteBuf &writeVarInt64(std::int64_t value)
    {
        return writeUnsignedVarInt64(static_cast<std::uint64_t>(value) << 1U ^ static_cast<std::uint64_t>(value >> 63));
    }
    ByteBuf &writeString(std::string_view value)
    {
        if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::out_of_range(std::format("string length: {} (expected: <= 4294967295)", value.size()));
        }
        writeUnsignedVarInt(static_cast<std::uint32_t>(value.size()));
        return writeBytes(std::as_bytes(std::span{value}));
    }

    [[nodiscard]] std::span<std::byte> nioBuffer() { return {memory() + reader_index_, readableBytes()}; }
    [[nodiscard]] std::span<const std::byte> nioBuffer() const { return {memory() + reader_index_, readableBytes()}; }
    [[nodiscard]] std::span<std::byte> nioBuffer(std::size_t index, std::size_t length);
    [[nodiscard]] std::span<const std::byte> nioBuffer(std::size_t index, std::size_t length) const;

private:
    friend class AbstractByteBuf;

    [[nodiscard]] std::byte *memory() { return array_.get(); }
    [[nodiscard]] const std::byte *memory() const { return array_.get(); }

    /**
     * @see io.netty.buffer.AbstractByteBufAllocator#calculateNewCapacity
     */
    [[nodiscard]] static std::size_t calculateNewCapacity(std::size_t min_new_capacity, std::size_t max_capacity);

    template <typename T, std::endian Order>
    ByteBuf &setValue(std::size_t index, T value)
    {
        checkIndex(index, sizeof(T));
        if constexpr (Order != std::endian::native) {
            value = std::byteswap(value);
        }
        std::memcpy(memory() + index, &value, sizeof(T));
        return *this;
    }

    template <typename T, std::endian Order>
    ByteBuf &writeValue(T value)
    {
        ensureWritable(sizeof(T));
        setValue<T, Order>(writer_index_, value);
        writer_index_ += sizeof(T);
        return *this;
    }

    std::unique_ptr<std::byte[]> array_;
    std::size_t capacity_{0};
    std::size_t max_capacity_{DEFAULT_MAX_CAPACITY};
    std::size_t marked_writer_index_{0};
};

/**
 * Bytes borrowed from a ByteBuf or an array, with a reader index of their own. It must not outlive what it borrows,
 * and growing the ByteBuf it came from invalidates it.
 *
 * @see io.netty.buffer.ByteBuf#slice
 * @see io.netty.buffer.Unpooled#wrappedBuffer
 */
class ByteBufView : public AbstractByteBuf {
public:
    ByteBufView() = default;
    explicit ByteBufView(std::span<const std::byte> array) : AbstractByteBuf(0, array.size()), array_(array.data()) {}

    [[nodiscard]] std::size_t capacity() const { return writer_index_; }
    [[nodiscard]] std::span<const std::byte> nioBuffer() const { return {array_ + reader_index_, readableBytes()}; }
    [[nodiscard]] std::span<const std::byte> nioBuffer(std::size_t index, std::size_t length) const
    {
        checkIndex(index, length);
        return {array_ + index, length};
    }

private:
    friend class AbstractByteBuf;

    [[nodiscard]] const std::byte *memory() const { return array_; }

    const std::byte *array_{nullptr};
};

template <typename Self>
ByteBufView AbstractByteBuf::readSlice(this Self &self, std::size_t length)
{
    self.checkReadableBytes(length);
    auto slice = self.slice(self.reader_index_, length);
    self.reader_index_ += length;
    return slice;
}

template <typename Self>
ByteBufView AbstractByteBuf::slice(this const Self &self)
{
    return self.slice(self.reader_index_, self.readableBytes());
}

template <typename Self>
ByteBufView AbstractByteBuf::slice(this const Self &self, std::size_t index, std::size_t length)
{
    self.checkIndex(index, length);
    return ByteBufView{std::span{self.memory() + index, length}};
}

template <typename Self>
ByteBufView AbstractByteBuf::duplicate(this const Self &self)
{
    ByteBufView duplicate{std::span{self.memory(), self.writer_index_}};
    duplicate.reader_index_ = self.reader_index_;
    return duplicate;
}

template <typename Self>
ByteBuf AbstractByteBuf::copy(this const Self &self)
{
    return self.copy(self.reader_index_, self.readableBytes());
}

template <typename Self>
ByteBuf AbstractByteBuf::copy(this const Self &self, std::size_t index, std::size_t length)
{
    self.checkIndex(index, length);
    ByteBuf buf{length, DEFAULT_MAX_CAPACITY};
    buf.writeBytes(std::span{self.memory() + index, length});
    return buf;
}

template <typename L, typename R>
    requires std::derived_from<L, AbstractByteBuf> && std::derived_from<R, AbstractByteBuf>
[[nodiscard]] bool operator==(const L &lhs, const R &rhs)
{
    return std::ranges::equal(lhs.nioBuffer(), rhs.nioBuffer());
}

/**
 * @see io.netty.buffer.Unpooled
 */
class Unpooled {
public:
    static constexpr std::size_t DEFAULT_INITIAL_CAPACITY = 256;

    [[nodiscard]] static ByteBuf buffer() { return buffer(DEFAULT_INITIAL_CAPACITY); }
    [[nodiscard]] static ByteBuf buffer(std::size_t initial_capacity)
    {
        return buffer(initial_capacity, AbstractByteBuf::DEFAULT_MAX_CAPACITY);
    }
    [[nodiscard]] static ByteBuf buffer(std::size_t initial_capacity, std::size_t max_capacity)
    {
        return ByteBuf{initial_capacity, max_capacity};
    }
    [[nodiscard]] static ByteBuf copiedBuffer(std::span<const std::byte> array)
    {
        auto buf = buffer(array.size());
        buf.writeBytes(array);
        return buf;
    }
    [[nodiscard]] static ByteBufView wrappedBuffer(std::span<const std::byte> array) { return ByteBufView{array}; }
};

/**
 * @see io.netty.buffer.ByteBufUtil
 */
class ByteBufUtil {
public:
    [[nodiscard]] static std::string hexDump(std::span<const std::byte> bytes);

    template <typename Buf>
        requires std::derived_from<Buf, AbstractByteBuf>
    [[nodiscard]] static std::string hexDump(const Buf &buffer)
    {
        return hexDump(buffer.nioBuffer());
    }
};

}  // namespace endgate
