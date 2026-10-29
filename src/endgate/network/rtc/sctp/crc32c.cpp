#include "endgate/network/rtc/sctp/crc32c.h"

#include <array>
#include <cstring>

#ifdef __x86_64__
#include <nmmintrin.h>
#endif

namespace endgate::rtc::sctp {

namespace {

std::uint32_t crc32cTable(std::span<const std::byte> data, std::uint32_t crc)
{
    static constexpr auto tables = [] {
        std::array<std::array<std::uint32_t, 256>, 8> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto entry = i;
            for (auto bit = 0; bit < 8; ++bit) {
                entry = (entry & 1U) ? (entry >> 1U) ^ 0x82F63B78U : entry >> 1U;
            }
            result[0][i] = entry;
        }
        for (std::size_t table = 1; table < result.size(); ++table) {
            for (std::size_t i = 0; i < 256; ++i) {
                const auto entry = result[table - 1][i];
                result[table][i] = (entry >> 8U) ^ result[0][entry & 0xFFU];
            }
        }
        return result;
    }();

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
    return crc;
}

#ifdef __x86_64__
[[gnu::target("sse4.2")]] std::uint32_t crc32cSse42(std::span<const std::byte> data, std::uint32_t crc)
{
    std::uint64_t wide = crc;
    auto remaining = data;
    while (remaining.size() >= 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, remaining.data(), sizeof(word));
        wide = _mm_crc32_u64(wide, word);
        remaining = remaining.subspan(8);
    }
    crc = static_cast<std::uint32_t>(wide);
    for (const auto byte : remaining) {
        crc = _mm_crc32_u8(crc, std::to_integer<std::uint8_t>(byte));
    }
    return crc;
}
#endif

}  // namespace

std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t previous)
{
#ifdef __x86_64__
#ifdef __SSE4_2__
    return ~crc32cSse42(data, ~previous);
#else
    static const bool hardware = __builtin_cpu_supports("sse4.2");
    if (hardware) {
        return ~crc32cSse42(data, ~previous);
    }
#endif
#endif
    return ~crc32cTable(data, ~previous);
}

}  // namespace endgate::rtc::sctp
