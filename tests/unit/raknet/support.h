#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace endgate::raknet::test {

inline std::vector<std::byte> fromHex(std::string_view hex)
{
    std::vector<std::byte> bytes;
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

}  // namespace endgate::raknet::test
