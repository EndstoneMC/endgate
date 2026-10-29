#include "endgate/buffer/bytebuf.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace endgate {

ByteBuf::ByteBuf(std::size_t initial_capacity, std::size_t max_capacity)
    : capacity_(initial_capacity), max_capacity_(max_capacity)
{
    if (initial_capacity > max_capacity) {
        throw std::invalid_argument(
            std::format("initialCapacity({}) > maxCapacity({})", initial_capacity, max_capacity));
    }
    if (initial_capacity > 0) {
        array_ = std::make_unique_for_overwrite<std::byte[]>(initial_capacity);
    }
}

ByteBuf::ByteBuf(ByteBuf &&other) noexcept
    : AbstractByteBuf(std::exchange(other.reader_index_, 0), std::exchange(other.writer_index_, 0)),
      array_(std::move(other.array_)), capacity_(std::exchange(other.capacity_, 0)), max_capacity_(other.max_capacity_),
      marked_writer_index_(std::exchange(other.marked_writer_index_, 0))
{
    marked_reader_index_ = std::exchange(other.marked_reader_index_, 0);
}

ByteBuf &ByteBuf::operator=(ByteBuf &&other) noexcept
{
    if (this != &other) {
        reader_index_ = std::exchange(other.reader_index_, 0);
        writer_index_ = std::exchange(other.writer_index_, 0);
        marked_reader_index_ = std::exchange(other.marked_reader_index_, 0);
        array_ = std::move(other.array_);
        capacity_ = std::exchange(other.capacity_, 0);
        max_capacity_ = other.max_capacity_;
        marked_writer_index_ = std::exchange(other.marked_writer_index_, 0);
    }
    return *this;
}

ByteBuf &ByteBuf::capacity(std::size_t new_capacity)
{
    if (new_capacity > max_capacity_) {
        throw std::invalid_argument(std::format("newCapacity: {} (expected: 0-{})", new_capacity, max_capacity_));
    }
    if (new_capacity == capacity_) {
        return *this;
    }
    std::unique_ptr<std::byte[]> array;
    if (new_capacity > 0) {
        array = std::make_unique_for_overwrite<std::byte[]>(new_capacity);
        if (const auto preserved = std::min(capacity_, new_capacity); preserved > 0) {
            std::memcpy(array.get(), array_.get(), preserved);
        }
    }
    array_ = std::move(array);
    capacity_ = new_capacity;
    if (writer_index_ > new_capacity) {
        writer_index_ = new_capacity;
        reader_index_ = std::min(reader_index_, new_capacity);
    }
    return *this;
}

ByteBuf &ByteBuf::writerIndex(std::size_t writer_index)
{
    if (writer_index < reader_index_ || writer_index > capacity_) {
        throw std::out_of_range(
            std::format("writerIndex: {} (expected: readerIndex({}) <= writerIndex <= capacity({}))", writer_index,
                        reader_index_, capacity_));
    }
    writer_index_ = writer_index;
    return *this;
}

ByteBuf &ByteBuf::setIndex(std::size_t reader_index, std::size_t writer_index)
{
    if (reader_index > writer_index || writer_index > capacity_) {
        throw std::out_of_range(
            std::format("readerIndex: {}, writerIndex: {} (expected: 0 <= readerIndex <= writerIndex <= capacity({}))",
                        reader_index, writer_index, capacity_));
    }
    reader_index_ = reader_index;
    writer_index_ = writer_index;
    return *this;
}

ByteBuf &ByteBuf::markWriterIndex()
{
    marked_writer_index_ = writer_index_;
    return *this;
}

ByteBuf &ByteBuf::resetWriterIndex()
{
    return writerIndex(marked_writer_index_);
}

ByteBuf &ByteBuf::clear()
{
    reader_index_ = 0;
    writer_index_ = 0;
    return *this;
}

ByteBuf &ByteBuf::discardReadBytes()
{
    if (reader_index_ == 0) {
        return *this;
    }
    const auto decrement = reader_index_;
    if (reader_index_ != writer_index_) {
        std::memmove(memory(), memory() + reader_index_, writer_index_ - reader_index_);
    }
    writer_index_ -= decrement;
    reader_index_ = 0;
    if (marked_reader_index_ <= decrement) {
        marked_reader_index_ = 0;
        marked_writer_index_ = marked_writer_index_ <= decrement ? 0 : marked_writer_index_ - decrement;
    }
    else {
        marked_reader_index_ -= decrement;
        marked_writer_index_ -= decrement;
    }
    return *this;
}

ByteBuf &ByteBuf::ensureWritable(std::size_t min_writable_bytes)
{
    if (min_writable_bytes <= writableBytes()) {
        return *this;
    }
    if (min_writable_bytes > maxWritableBytes()) {
        throw std::out_of_range(std::format("writerIndex({}) + minWritableBytes({}) exceeds maxCapacity({})",
                                            writer_index_, min_writable_bytes, max_capacity_));
    }
    return capacity(calculateNewCapacity(writer_index_ + min_writable_bytes, max_capacity_));
}

ByteBuf &ByteBuf::setMedium(std::size_t index, std::uint32_t value)
{
    checkIndex(index, 3);
    auto *bytes = memory() + index;
    bytes[0] = static_cast<std::byte>(value >> 16U);
    bytes[1] = static_cast<std::byte>(value >> 8U);
    bytes[2] = static_cast<std::byte>(value);
    return *this;
}

ByteBuf &ByteBuf::setMediumLE(std::size_t index, std::uint32_t value)
{
    checkIndex(index, 3);
    auto *bytes = memory() + index;
    bytes[0] = static_cast<std::byte>(value);
    bytes[1] = static_cast<std::byte>(value >> 8U);
    bytes[2] = static_cast<std::byte>(value >> 16U);
    return *this;
}

ByteBuf &ByteBuf::setBytes(std::size_t index, std::span<const std::byte> src)
{
    checkIndex(index, src.size());
    if (!src.empty()) {
        std::memmove(memory() + index, src.data(), src.size());
    }
    return *this;
}

ByteBuf &ByteBuf::setZero(std::size_t index, std::size_t length)
{
    checkIndex(index, length);
    if (length > 0) {
        std::memset(memory() + index, 0, length);
    }
    return *this;
}

ByteBuf &ByteBuf::writeMedium(std::uint32_t value)
{
    ensureWritable(3);
    setMedium(writer_index_, value);
    writer_index_ += 3;
    return *this;
}

ByteBuf &ByteBuf::writeMediumLE(std::uint32_t value)
{
    ensureWritable(3);
    setMediumLE(writer_index_, value);
    writer_index_ += 3;
    return *this;
}

ByteBuf &ByteBuf::writeBytes(std::span<const std::byte> src)
{
    ensureWritable(src.size());
    setBytes(writer_index_, src);
    writer_index_ += src.size();
    return *this;
}

ByteBuf &ByteBuf::writeBytes(ByteBufView &src)
{
    const auto bytes = src.nioBuffer();
    writeBytes(bytes);
    src.skipBytes(bytes.size());
    return *this;
}

ByteBuf &ByteBuf::writeBytes(const ByteBufView &src, std::size_t src_index, std::size_t length)
{
    return writeBytes(src.nioBuffer(src_index, length));
}

ByteBuf &ByteBuf::writeZero(std::size_t length)
{
    ensureWritable(length);
    setZero(writer_index_, length);
    writer_index_ += length;
    return *this;
}

std::span<std::byte> ByteBuf::nioBuffer(std::size_t index, std::size_t length)
{
    checkIndex(index, length);
    return {memory() + index, length};
}

std::span<const std::byte> ByteBuf::nioBuffer(std::size_t index, std::size_t length) const
{
    checkIndex(index, length);
    return {memory() + index, length};
}

std::size_t ByteBuf::calculateNewCapacity(std::size_t min_new_capacity, std::size_t max_capacity)
{
    constexpr std::size_t threshold = 1048576 * 4;
    if (min_new_capacity > max_capacity) {
        throw std::invalid_argument(std::format("minNewCapacity: {} (expected: not greater than maxCapacity({})",
                                                min_new_capacity, max_capacity));
    }
    if (min_new_capacity == threshold) {
        return threshold;
    }
    if (min_new_capacity > threshold) {
        const auto new_capacity = min_new_capacity / threshold * threshold;
        return new_capacity > max_capacity - threshold ? max_capacity : new_capacity + threshold;
    }
    return std::min(std::bit_ceil(std::max<std::size_t>(min_new_capacity, 64)), max_capacity);
}

std::string ByteBufUtil::hexDump(std::span<const std::byte> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string dump;
    dump.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        dump.push_back(digits[value >> 4U]);
        dump.push_back(digits[value & 0x0FU]);
    }
    return dump;
}

}  // namespace endgate
