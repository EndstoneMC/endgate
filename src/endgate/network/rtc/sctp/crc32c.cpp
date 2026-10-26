#include "endgate/network/rtc/sctp/crc32c.h"

#include <array>

namespace endgate::rtc::sctp {

std::uint32_t crc32c(std::span<const std::byte> data)
{
    static constexpr auto tables = [] {
        std::array<std::array<std::uint32_t, 256>, 8> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto crc = i;
            for (auto bit = 0; bit < 8; ++bit) {
                crc = (crc & 1U) ? (crc >> 1U) ^ 0x82F63B78U : crc >> 1U;
            }
            result[0][i] = crc;
        }
        for (std::size_t table = 1; table < result.size(); ++table) {
            for (std::size_t i = 0; i < 256; ++i) {
                const auto previous = result[table - 1][i];
                result[table][i] = (previous >> 8U) ^ result[0][previous & 0xFFU];
            }
        }
        return result;
    }();

    std::uint32_t crc = 0xFFFFFFFFU;
    auto remaining = data;
    while (remaining.size() >= 8) {
        const auto low =
            crc ^
            (std::to_integer<std::uint32_t>(remaining[0]) | std::to_integer<std::uint32_t>(remaining[1]) << 8U |
             std::to_integer<std::uint32_t>(remaining[2]) << 16U | std::to_integer<std::uint32_t>(remaining[3]) << 24U);
        crc = tables[7][low & 0xFFU] ^ tables[6][(low >> 8U) & 0xFFU] ^ tables[5][(low >> 16U) & 0xFFU] ^
              tables[4][low >> 24U] ^ tables[3][std::to_integer<std::uint8_t>(remaining[4])] ^
              tables[2][std::to_integer<std::uint8_t>(remaining[5])] ^
              tables[1][std::to_integer<std::uint8_t>(remaining[6])] ^
              tables[0][std::to_integer<std::uint8_t>(remaining[7])];
        remaining = remaining.subspan(8);
    }
    for (const auto byte : remaining) {
        crc = (crc >> 8U) ^ tables[0][(crc ^ std::to_integer<std::uint32_t>(byte)) & 0xFFU];
    }
    return ~crc;
}

}  // namespace endgate::rtc::sctp
