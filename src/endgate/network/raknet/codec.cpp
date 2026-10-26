#include "endgate/network/raknet/codec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <ranges>

#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>
#include <sys/socket.h>

namespace endgate::raknet {

std::optional<std::span<const std::byte>> Reader::readBytes(std::size_t count)
{
    if (count > remaining()) {
        return std::nullopt;
    }
    const auto bytes = data_.subspan(offset_, count);
    offset_ += count;
    return bytes;
}

std::span<const std::byte> Reader::readRemaining()
{
    const auto bytes = data_.subspan(offset_);
    offset_ = data_.size();
    return bytes;
}

bool Reader::skip(std::size_t count)
{
    return readBytes(count).has_value();
}

std::optional<std::uint8_t> Reader::readUint8()
{
    const auto bytes = readBytes(1);
    if (!bytes) {
        return std::nullopt;
    }
    return std::to_integer<std::uint8_t>((*bytes)[0]);
}

std::optional<std::uint16_t> Reader::readUint16()
{
    const auto bytes = readBytes(2);
    if (!bytes) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>((*bytes)[0]) << 8U |
                                      std::to_integer<std::uint16_t>((*bytes)[1]));
}

std::optional<std::uint32_t> Reader::readUint24()
{
    const auto bytes = readBytes(3);
    if (!bytes) {
        return std::nullopt;
    }
    return std::to_integer<std::uint32_t>((*bytes)[0]) | std::to_integer<std::uint32_t>((*bytes)[1]) << 8U |
           std::to_integer<std::uint32_t>((*bytes)[2]) << 16U;
}

std::optional<std::uint32_t> Reader::readUint32()
{
    const auto bytes = readBytes(4);
    if (!bytes) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const auto byte : *bytes) {
        value = value << 8U | std::to_integer<std::uint32_t>(byte);
    }
    return value;
}

std::optional<std::uint64_t> Reader::readUint64()
{
    const auto bytes = readBytes(8);
    if (!bytes) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const auto byte : *bytes) {
        value = value << 8U | std::to_integer<std::uint64_t>(byte);
    }
    return value;
}

std::optional<float> Reader::readFloat()
{
    const auto bits = readUint32();
    if (!bits) {
        return std::nullopt;
    }
    return std::bit_cast<float>(*bits);
}

std::optional<SystemAddress> Reader::readAddress()
{
    const auto version = readUint8();
    if (!version) {
        return std::nullopt;
    }
    if (*version == 4) {
        const auto inverted = readBytes(4);
        const auto port = readUint16();
        if (!inverted || !port) {
            return std::nullopt;
        }
        asio::ip::address_v4::bytes_type bytes{};
        std::ranges::transform(*inverted, bytes.begin(),
                               [](std::byte byte) { return std::to_integer<unsigned char>(~byte); });
        return SystemAddress{asio::ip::address_v4{bytes}, *port};
    }
    const auto family = readBytes(2);
    const auto port = readUint16();
    const auto flow_info = readBytes(4);
    const auto address = readBytes(16);
    const auto scope_id = readBytes(4);
    if (!family || !port || !flow_info || !address || !scope_id) {
        return std::nullopt;
    }
    asio::ip::address_v6::bytes_type bytes{};
    std::ranges::transform(*address, bytes.begin(),
                           [](std::byte byte) { return std::to_integer<unsigned char>(byte); });
    std::uint32_t scope = 0;
    for (const auto byte : *scope_id | std::views::reverse) {
        scope = scope << 8U | std::to_integer<std::uint32_t>(byte);
    }
    return SystemAddress{asio::ip::address_v6{bytes, scope}, *port};
}

std::span<std::byte> Writer::claim(std::size_t count)
{
    if (overflowed_ || count > buffer_.size() - size_) {
        overflowed_ = true;
        return {};
    }
    const auto claimed = buffer_.subspan(size_, count);
    size_ += count;
    return claimed;
}

void Writer::writeBytes(std::span<const std::byte> bytes)
{
    const auto target = claim(bytes.size());
    if (target.size() == bytes.size()) {
        std::ranges::copy(bytes, target.begin());
    }
}

void Writer::writeZeros(std::size_t count)
{
    std::ranges::fill(claim(count), std::byte{0});
}

void Writer::writeUint8(std::uint8_t value)
{
    writeBytes(std::array{std::byte{value}});
}

void Writer::writeUint16(std::uint16_t value)
{
    writeBytes(std::array{static_cast<std::byte>(value >> 8U), static_cast<std::byte>(value)});
}

void Writer::writeUint24(std::uint32_t value)
{
    writeBytes(std::array{static_cast<std::byte>(value), static_cast<std::byte>(value >> 8U),
                          static_cast<std::byte>(value >> 16U)});
}

void Writer::writeUint32(std::uint32_t value)
{
    writeBytes(std::array{static_cast<std::byte>(value >> 24U), static_cast<std::byte>(value >> 16U),
                          static_cast<std::byte>(value >> 8U), static_cast<std::byte>(value)});
}

void Writer::writeUint64(std::uint64_t value)
{
    writeUint32(static_cast<std::uint32_t>(value >> 32U));
    writeUint32(static_cast<std::uint32_t>(value));
}

void Writer::writeFloat(float value)
{
    writeUint32(std::bit_cast<std::uint32_t>(value));
}

void Writer::writeAddress(const SystemAddress &address)
{
    if (address.address().is_v4()) {
        writeUint8(4);
        for (const auto byte : address.address().to_v4().to_bytes()) {
            writeUint8(static_cast<std::uint8_t>(~byte));
        }
        writeUint16(address.port());
        return;
    }
    const auto v6 = address.address().to_v6();
    const auto scope_id = static_cast<std::uint32_t>(v6.scope_id());
    writeUint8(6);
    writeBytes(std::array{static_cast<std::byte>(AF_INET6 & 0xFF), static_cast<std::byte>(AF_INET6 >> 8)});
    writeUint16(address.port());
    writeZeros(4);
    for (const auto byte : v6.to_bytes()) {
        writeUint8(byte);
    }
    writeBytes(std::array{static_cast<std::byte>(scope_id), static_cast<std::byte>(scope_id >> 8U),
                          static_cast<std::byte>(scope_id >> 16U), static_cast<std::byte>(scope_id >> 24U)});
}

}  // namespace endgate::raknet
