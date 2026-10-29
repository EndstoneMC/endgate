#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "endgate/buffer/bytebuf.h"

namespace endgate::raknet::test {

struct Bytes : std::vector<std::byte> {
    using std::vector<std::byte>::vector;

    operator ByteBufView() const { return Unpooled::wrappedBuffer(*this); }
};

inline Bytes fromHex(std::string_view hex)
{
    Bytes bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<std::byte>(std::stoi(std::string{hex.substr(i, 2)}, nullptr, 16)));
    }
    return bytes;
}

inline std::string toHex(std::span<const std::byte> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    for (const auto byte : bytes) {
        hex.push_back(digits[std::to_integer<unsigned>(byte) >> 4U]);
        hex.push_back(digits[std::to_integer<unsigned>(byte) & 0x0FU]);
    }
    return hex;
}

inline std::string toHex(const ByteBufView &view)
{
    return toHex(view.nioBuffer());
}

inline std::string toHex(const ByteBuf &buffer)
{
    return toHex(buffer.nioBuffer());
}

}  // namespace endgate::raknet::test
