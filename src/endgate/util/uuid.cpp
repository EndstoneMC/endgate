#include "endgate/util/uuid.h"

#include <charconv>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace endgate {

UUID UUID::nameUUIDFromBytes(std::span<const std::byte> name)
{
    UUID uuid;
    unsigned int length = 0;
    EVP_Digest(name.data(), name.size(), uuid.bytes.data(), &length, EVP_md5(), nullptr);
    uuid.bytes[6] = static_cast<std::uint8_t>((uuid.bytes[6] & 0x0f) | 0x30);
    uuid.bytes[8] = static_cast<std::uint8_t>((uuid.bytes[8] & 0x3f) | 0x80);
    return uuid;
}

UUID UUID::randomUUID()
{
    UUID uuid;
    RAND_bytes(uuid.bytes.data(), static_cast<int>(uuid.bytes.size()));
    uuid.bytes[6] = static_cast<std::uint8_t>((uuid.bytes[6] & 0x0f) | 0x40);
    uuid.bytes[8] = static_cast<std::uint8_t>((uuid.bytes[8] & 0x3f) | 0x80);
    return uuid;
}

std::optional<UUID> UUID::fromString(std::string_view name)
{
    constexpr std::size_t length = 36;

    if (name.size() != length) {
        return std::nullopt;
    }
    UUID uuid;
    std::size_t position = 0;
    for (std::size_t i = 0; i < uuid.bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            if (name[position++] != '-') {
                return std::nullopt;
            }
        }
        const auto *const begin = name.data() + position;
        if (std::from_chars(begin, begin + 2, uuid.bytes[i], 16) !=
            std::from_chars_result{.ptr = begin + 2, .ec = std::errc{}}) {
            return std::nullopt;
        }
        position += 2;
    }
    return uuid;
}

std::uint64_t UUID::getMostSignificantBits() const
{
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        bits = (bits << 8) | bytes[i];
    }
    return bits;
}

std::uint64_t UUID::getLeastSignificantBits() const
{
    std::uint64_t bits = 0;
    for (std::size_t i = 8; i < 16; ++i) {
        bits = (bits << 8) | bytes[i];
    }
    return bits;
}

std::string UUID::toString() const
{
    std::string text;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            text += '-';
        }
        text += std::format("{:02x}", bytes[i]);
    }
    return text;
}

UUID UuidUtils::generateOfflinePlayerUuid(std::string_view username)
{
    const auto name = "OfflinePlayer:" + std::string(username);
    return UUID::nameUUIDFromBytes(std::as_bytes(std::span{name}));
}

UUID UuidUtils::generateXuidPlayerUuid(std::string_view xuid)
{
    const auto name = "pocket-auth-1-xuid:" + std::string(xuid);
    return UUID::nameUUIDFromBytes(std::as_bytes(std::span{name}));
}

}  // namespace endgate
