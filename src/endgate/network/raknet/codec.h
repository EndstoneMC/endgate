#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <asio/ip/udp.hpp>

namespace endgate::raknet {

/**
 * @see RakNet::SystemAddress
 */
using SystemAddress = asio::ip::udp::endpoint;

/**
 * @see RakNet::BitStream
 */
class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] std::size_t remaining() const { return data_.size() - offset_; }
    [[nodiscard]] std::size_t offset() const { return offset_; }

    [[nodiscard]] std::optional<std::span<const std::byte>> readBytes(std::size_t count);
    [[nodiscard]] std::span<const std::byte> readRemaining();
    [[nodiscard]] bool skip(std::size_t count);

    [[nodiscard]] std::optional<std::uint8_t> readUint8();
    [[nodiscard]] std::optional<std::uint16_t> readUint16();
    [[nodiscard]] std::optional<std::uint32_t> readUint24();
    [[nodiscard]] std::optional<std::uint32_t> readUint32();
    [[nodiscard]] std::optional<std::uint64_t> readUint64();
    [[nodiscard]] std::optional<float> readFloat();

    /**
     * @see RakNet::BitStream::Read(SystemAddress &)
     */
    [[nodiscard]] std::optional<SystemAddress> readAddress();

private:
    std::span<const std::byte> data_;
    std::size_t offset_{0};
};

/**
 * @see RakNet::BitStream
 */
class Writer {
public:
    explicit Writer(std::span<std::byte> buffer) : buffer_(buffer) {}

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool overflowed() const { return overflowed_; }
    [[nodiscard]] std::span<const std::byte> written() const { return buffer_.first(size_); }

    void writeBytes(std::span<const std::byte> bytes);
    void writeZeros(std::size_t count);
    [[nodiscard]] std::span<std::byte> claim(std::size_t count);

    void writeUint8(std::uint8_t value);
    void writeUint16(std::uint16_t value);
    void writeUint24(std::uint32_t value);
    void writeUint32(std::uint32_t value);
    void writeUint64(std::uint64_t value);
    void writeFloat(float value);

    /**
     * @see RakNet::BitStream::Write(const SystemAddress &)
     */
    void writeAddress(const SystemAddress &address);

private:
    std::span<std::byte> buffer_;
    std::size_t size_{0};
    bool overflowed_{false};
};

}  // namespace endgate::raknet
