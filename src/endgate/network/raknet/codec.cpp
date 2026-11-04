#include "endgate/network/raknet/codec.h"

#include <algorithm>
#include <array>
#include <span>

#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace endgate::raknet {

bool isAddressReadable(const ByteBufView &buffer)
{
    constexpr std::size_t ipv4_size = 1 + 4 + 2;
    constexpr std::size_t ipv6_size = 1 + 2 + 2 + 4 + 16 + 4;

    if (!buffer.isReadable()) {
        return false;
    }
    return buffer.isReadable(buffer.getUnsignedByte(buffer.readerIndex()) == 4 ? ipv4_size : ipv6_size);
}

SystemAddress readAddress(ByteBufView &buffer)
{
    const auto version = buffer.readUnsignedByte();
    if (version == 4) {
        asio::ip::address_v4::bytes_type bytes{};
        buffer.readBytes(std::as_writable_bytes(std::span{bytes}));
        std::ranges::transform(bytes, bytes.begin(),
                               [](unsigned char byte) { return static_cast<unsigned char>(~byte); });
        return SystemAddress{asio::ip::address_v4{bytes}, buffer.readUnsignedShort()};
    }
    buffer.skipBytes(2);
    const auto port = buffer.readUnsignedShort();
    buffer.skipBytes(4);
    asio::ip::address_v6::bytes_type bytes{};
    buffer.readBytes(std::as_writable_bytes(std::span{bytes}));
    const auto scope_id = buffer.readUnsignedIntLE();
    return SystemAddress{asio::ip::address_v6{bytes, scope_id}, port};
}

void writeAddress(ByteBuf &buffer, const SystemAddress &address)
{
    if (address.address().is_v4()) {
        buffer.writeByte(4);
        for (const auto byte : address.address().to_v4().to_bytes()) {
            buffer.writeByte(static_cast<std::uint8_t>(~byte));
        }
        buffer.writeShort(address.port());
        return;
    }
    const auto v6 = address.address().to_v6();
    buffer.writeByte(6);
    buffer.writeShortLE(AF_INET6);
    buffer.writeShort(address.port());
    buffer.writeZero(4);
    const auto bytes = v6.to_bytes();
    buffer.writeBytes(std::as_bytes(std::span{bytes}));
    buffer.writeIntLE(static_cast<std::uint32_t>(v6.scope_id()));
}

}  // namespace endgate::raknet
