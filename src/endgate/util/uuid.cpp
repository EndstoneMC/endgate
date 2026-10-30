#include "endgate/util/uuid.h"

#include <cstddef>
#include <format>
#include <string>

#include <openssl/evp.h>

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
