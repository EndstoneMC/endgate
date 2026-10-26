#include "endgate/network/rtc/stun.h"

#include <libdeflate.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <ranges>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>

namespace endgate::rtc {

namespace {

constexpr std::uint32_t MagicCookie = 0x2112A442;
constexpr std::size_t HeaderSize = 20;
constexpr std::size_t AttributeHeaderSize = 4;
constexpr std::size_t IntegritySize = 20;
constexpr std::size_t MaxUsernameLength = 513;
constexpr std::size_t MaxTextLength = 763;
constexpr std::size_t MaxUnknownAttributes = 16;

std::uint16_t readU16(std::span<const std::byte> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(bytes[offset]) << 8U) |
                                      std::to_integer<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t readU32(std::span<const std::byte> bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(readU16(bytes, offset)) << 16U) | readU16(bytes, offset + 2);
}

void writeU16(std::vector<std::byte> &out, std::uint16_t value)
{
    out.push_back(static_cast<std::byte>(value >> 8U));
    out.push_back(static_cast<std::byte>(value));
}

void writeU32(std::vector<std::byte> &out, std::uint32_t value)
{
    writeU16(out, static_cast<std::uint16_t>(value >> 16U));
    writeU16(out, static_cast<std::uint16_t>(value));
}

void writeAttribute(std::vector<std::byte> &out, StunAttribute type, std::span<const std::byte> value)
{
    writeU16(out, static_cast<std::uint16_t>(type));
    writeU16(out, static_cast<std::uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
    out.resize(out.size() + ((4 - value.size() % 4) % 4), std::byte{0});
}

std::span<const std::byte> asBytes(std::string_view text)
{
    return std::as_bytes(std::span{text.data(), text.size()});
}

std::array<std::byte, 16> xorMask(const StunMessage::TransactionId &transaction_id)
{
    std::array<std::byte, 16> mask{};
    mask[0] = static_cast<std::byte>(MagicCookie >> 24U);
    mask[1] = static_cast<std::byte>(MagicCookie >> 16U);
    mask[2] = static_cast<std::byte>(MagicCookie >> 8U);
    mask[3] = static_cast<std::byte>(MagicCookie);
    std::ranges::copy(transaction_id, mask.begin() + 4);
    return mask;
}

std::uint32_t fingerprintOf(std::span<const std::byte> bytes)
{
    return static_cast<std::uint32_t>(libdeflate_crc32(0, bytes.data(), bytes.size())) ^ 0x5354554EU;
}

std::array<std::byte, IntegritySize> hmacSha1(std::span<const std::byte> header, std::span<const std::byte> body,
                                              std::string_view key)
{
    static EVP_MAC *const mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    std::array<std::byte, IntegritySize> digest{};
    const std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)> context(EVP_MAC_CTX_new(mac), EVP_MAC_CTX_free);
    std::array<char, 5> digest_name{"SHA1"};
    const std::array params{OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name.data(), 0),
                            OSSL_PARAM_construct_end()};
    std::size_t length = 0;
    if (!context ||
        EVP_MAC_init(context.get(), reinterpret_cast<const unsigned char *>(key.data()), key.size(), params.data()) !=
            1 ||
        EVP_MAC_update(context.get(), reinterpret_cast<const unsigned char *>(header.data()), header.size()) != 1 ||
        EVP_MAC_update(context.get(), reinterpret_cast<const unsigned char *>(body.data()), body.size()) != 1 ||
        EVP_MAC_final(context.get(), reinterpret_cast<unsigned char *>(digest.data()), &length, digest.size()) != 1) {
        return {};
    }
    return digest;
}

std::optional<asio::ip::udp::endpoint> readXorAddress(std::span<const std::byte> value,
                                                      const StunMessage::TransactionId &transaction_id)
{
    const auto mask = xorMask(transaction_id);
    const auto family = std::to_integer<std::uint8_t>(value[1]);
    const auto port = static_cast<std::uint16_t>(readU16(value, 2) ^ (MagicCookie >> 16U));
    if (family == 0x01 && value.size() == 8) {
        asio::ip::address_v4::bytes_type bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = std::to_integer<unsigned char>(value[4 + i] ^ mask[i]);
        }
        return asio::ip::udp::endpoint{asio::ip::address_v4{bytes}, port};
    }
    if (family == 0x02 && value.size() == 20) {
        asio::ip::address_v6::bytes_type bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = std::to_integer<unsigned char>(value[4 + i] ^ mask[i]);
        }
        return asio::ip::udp::endpoint{asio::ip::address_v6{bytes}, port};
    }
    return std::nullopt;
}

bool isHeaderValid(std::span<const std::byte> datagram)
{
    if (datagram.size() < HeaderSize || (std::to_integer<std::uint8_t>(datagram[0]) & 0xC0U) != 0) {
        return false;
    }
    const auto length = readU16(datagram, 2);
    return readU32(datagram, 4) == MagicCookie && length % 4 == 0 && datagram.size() == HeaderSize + length;
}

}  // namespace

std::optional<StunMessage> StunMessage::decode(std::span<const std::byte> datagram)
{
    if (!isHeaderValid(datagram)) {
        return std::nullopt;
    }

    StunMessage message;
    const auto type = readU16(datagram, 0);
    message.message_class = static_cast<StunClass>(type & 0x0110U);
    message.method = static_cast<StunMethod>((type & 0x000FU) | ((type & 0x00E0U) >> 1U) | ((type & 0x3E00U) >> 2U));
    std::ranges::copy(datagram.subspan(8, 12), message.transaction_id.begin());

    bool after_integrity = false;
    bool after_integrity_sha256 = false;
    std::size_t offset = HeaderSize;
    while (offset < datagram.size()) {
        if (message.fingerprint || datagram.size() - offset < AttributeHeaderSize) {
            return std::nullopt;
        }
        const auto attribute = readU16(datagram, offset);
        const std::size_t length = readU16(datagram, offset + 2);
        const auto padded = (length + 3) & ~std::size_t{3};
        if (datagram.size() - offset - AttributeHeaderSize < padded) {
            return std::nullopt;
        }
        const auto value = datagram.subspan(offset + AttributeHeaderSize, length);
        const auto attribute_offset = offset;
        offset += AttributeHeaderSize + padded;

        if (attribute == static_cast<std::uint16_t>(StunAttribute::Fingerprint)) {
            if (length != 4 || readU32(value, 0) != fingerprintOf(datagram.first(attribute_offset))) {
                return std::nullopt;
            }
            message.fingerprint = true;
            continue;
        }
        if (after_integrity_sha256) {
            continue;
        }
        if (attribute == static_cast<std::uint16_t>(StunAttribute::MessageIntegritySha256)) {
            if (length < 16 || length > 32 || length % 4 != 0) {
                return std::nullopt;
            }
            after_integrity_sha256 = true;
            continue;
        }
        if (after_integrity) {
            continue;
        }

        switch (static_cast<StunAttribute>(attribute)) {
        case StunAttribute::Username:
            if (length > MaxUsernameLength) {
                return std::nullopt;
            }
            if (!message.username) {
                message.username.emplace(reinterpret_cast<const char *>(value.data()), value.size());
            }
            break;
        case StunAttribute::MessageIntegrity:
            if (length != IntegritySize) {
                return std::nullopt;
            }
            message.integrity_offset = attribute_offset;
            after_integrity = true;
            break;
        case StunAttribute::ErrorCode:
            if (length < 4 || length - 4 > MaxTextLength) {
                return std::nullopt;
            }
            if (!message.error_code) {
                message.error_code = StunErrorCode{
                    .code = static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(value[2]) & 0x07U) * 100U +
                                                       std::to_integer<std::uint16_t>(value[3])),
                    .reason = std::string(reinterpret_cast<const char *>(value.data()) + 4, length - 4)};
            }
            break;
        case StunAttribute::UnknownAttributes:
            if (length % 2 != 0) {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < length && message.unknown_attributes.size() < MaxUnknownAttributes; i += 2) {
                message.unknown_attributes.push_back(readU16(value, i));
            }
            break;
        case StunAttribute::XorMappedAddress:
            if (length < 4) {
                return std::nullopt;
            }
            if (!message.xor_mapped_address) {
                message.xor_mapped_address = readXorAddress(value, message.transaction_id);
            }
            break;
        case StunAttribute::Priority:
            if (length != 4) {
                return std::nullopt;
            }
            if (!message.priority) {
                message.priority = readU32(value, 0);
            }
            break;
        case StunAttribute::UseCandidate:
            if (length != 0) {
                return std::nullopt;
            }
            message.use_candidate = true;
            break;
        case StunAttribute::Software:
            if (length > MaxTextLength) {
                return std::nullopt;
            }
            if (!message.software) {
                message.software.emplace(reinterpret_cast<const char *>(value.data()), value.size());
            }
            break;
        case StunAttribute::IceControlled:
        case StunAttribute::IceControlling: {
            if (length != 8) {
                return std::nullopt;
            }
            auto &tiebreaker = attribute == static_cast<std::uint16_t>(StunAttribute::IceControlled)
                                 ? message.ice_controlled
                                 : message.ice_controlling;
            if (!tiebreaker) {
                tiebreaker = (static_cast<std::uint64_t>(readU32(value, 0)) << 32U) | readU32(value, 4);
            }
            break;
        }
        default:
            if (attribute < 0x8000 && message.unknown_comprehension_required.size() < MaxUnknownAttributes &&
                !std::ranges::contains(message.unknown_comprehension_required, attribute)) {
                message.unknown_comprehension_required.push_back(attribute);
            }
            break;
        }
    }
    return message;
}

std::optional<std::string_view> StunMessage::peekLocalUfrag(std::span<const std::byte> datagram)
{
    if (!isHeaderValid(datagram) || readU16(datagram, 0) != static_cast<std::uint16_t>(StunMethod::Binding)) {
        return std::nullopt;
    }
    std::size_t offset = HeaderSize;
    while (datagram.size() - offset >= AttributeHeaderSize) {
        const auto attribute = readU16(datagram, offset);
        const std::size_t length = readU16(datagram, offset + 2);
        const auto padded = (length + 3) & ~std::size_t{3};
        if (datagram.size() - offset - AttributeHeaderSize < padded ||
            attribute == static_cast<std::uint16_t>(StunAttribute::MessageIntegrity)) {
            return std::nullopt;
        }
        if (attribute == static_cast<std::uint16_t>(StunAttribute::Username)) {
            if (length > MaxUsernameLength) {
                return std::nullopt;
            }
            const std::string_view username(
                reinterpret_cast<const char *>(datagram.data()) + offset + AttributeHeaderSize, length);
            const auto colon = username.find(':');
            if (colon == std::string_view::npos || colon == 0) {
                return std::nullopt;
            }
            return username.substr(0, colon);
        }
        offset += AttributeHeaderSize + padded;
    }
    return std::nullopt;
}

std::vector<std::byte> StunMessage::encode(std::optional<std::string_view> integrity_key) const
{
    std::vector<std::byte> out;
    out.reserve(128);
    const auto method_bits = static_cast<std::uint16_t>(method);
    writeU16(out,
             static_cast<std::uint16_t>((method_bits & 0x000FU) | ((method_bits & 0x0070U) << 1U) |
                                        ((method_bits & 0x0F80U) << 2U) | static_cast<std::uint16_t>(message_class)));
    writeU16(out, 0);
    writeU32(out, MagicCookie);
    out.insert(out.end(), transaction_id.begin(), transaction_id.end());

    if (xor_mapped_address) {
        const auto mask = xorMask(transaction_id);
        const auto address = xor_mapped_address->address();
        std::vector<std::byte> value;
        value.push_back(std::byte{0});
        value.push_back(address.is_v4() ? std::byte{0x01} : std::byte{0x02});
        writeU16(value, static_cast<std::uint16_t>(xor_mapped_address->port() ^ (MagicCookie >> 16U)));
        if (address.is_v4()) {
            const auto bytes = address.to_v4().to_bytes();
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                value.push_back(static_cast<std::byte>(bytes[i]) ^ mask[i]);
            }
        }
        else {
            const auto bytes = address.to_v6().to_bytes();
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                value.push_back(static_cast<std::byte>(bytes[i]) ^ mask[i]);
            }
        }
        writeAttribute(out, StunAttribute::XorMappedAddress, value);
    }
    if (error_code) {
        std::vector<std::byte> value{std::byte{0}, std::byte{0}, static_cast<std::byte>(error_code->code / 100),
                                     static_cast<std::byte>(error_code->code % 100)};
        const auto reason = asBytes(error_code->reason).first(std::min(error_code->reason.size(), MaxTextLength));
        value.insert(value.end(), reason.begin(), reason.end());
        writeAttribute(out, StunAttribute::ErrorCode, value);
    }
    if (!unknown_attributes.empty()) {
        std::vector<std::byte> value;
        for (const auto attribute : unknown_attributes | std::views::take(MaxUnknownAttributes)) {
            writeU16(value, attribute);
        }
        writeAttribute(out, StunAttribute::UnknownAttributes, value);
    }
    if (username) {
        writeAttribute(out, StunAttribute::Username,
                       asBytes(*username).first(std::min(username->size(), MaxUsernameLength)));
    }
    if (priority) {
        std::vector<std::byte> value;
        writeU32(value, *priority);
        writeAttribute(out, StunAttribute::Priority, value);
    }
    if (use_candidate) {
        writeAttribute(out, StunAttribute::UseCandidate, {});
    }
    if (ice_controlled) {
        std::vector<std::byte> value;
        writeU32(value, static_cast<std::uint32_t>(*ice_controlled >> 32U));
        writeU32(value, static_cast<std::uint32_t>(*ice_controlled));
        writeAttribute(out, StunAttribute::IceControlled, value);
    }
    if (ice_controlling) {
        std::vector<std::byte> value;
        writeU32(value, static_cast<std::uint32_t>(*ice_controlling >> 32U));
        writeU32(value, static_cast<std::uint32_t>(*ice_controlling));
        writeAttribute(out, StunAttribute::IceControlling, value);
    }
    if (software) {
        writeAttribute(out, StunAttribute::Software,
                       asBytes(*software).first(std::min(software->size(), MaxTextLength)));
    }

    if (integrity_key) {
        const auto length_with_integrity = out.size() - HeaderSize + AttributeHeaderSize + IntegritySize;
        out[2] = static_cast<std::byte>(length_with_integrity >> 8U);
        out[3] = static_cast<std::byte>(length_with_integrity);
        const std::span<const std::byte> bytes{out};
        const auto digest = hmacSha1(bytes.first(HeaderSize), bytes.subspan(HeaderSize), *integrity_key);
        writeAttribute(out, StunAttribute::MessageIntegrity, digest);
    }

    const auto length_with_fingerprint = out.size() - HeaderSize + AttributeHeaderSize + 4;
    out[2] = static_cast<std::byte>(length_with_fingerprint >> 8U);
    out[3] = static_cast<std::byte>(length_with_fingerprint);
    std::vector<std::byte> fingerprint_value;
    writeU32(fingerprint_value, fingerprintOf(out));
    writeAttribute(out, StunAttribute::Fingerprint, fingerprint_value);
    return out;
}

bool StunMessage::verifyIntegrity(std::span<const std::byte> datagram, std::string_view key) const
{
    if (!integrity_offset || *integrity_offset + AttributeHeaderSize + IntegritySize > datagram.size()) {
        return false;
    }
    std::array<std::byte, HeaderSize> header{};
    std::ranges::copy(datagram.first(HeaderSize), header.begin());
    const auto length = *integrity_offset - HeaderSize + AttributeHeaderSize + IntegritySize;
    header[2] = static_cast<std::byte>(length >> 8U);
    header[3] = static_cast<std::byte>(length);
    const auto digest = hmacSha1(header, datagram.subspan(HeaderSize, *integrity_offset - HeaderSize), key);
    const auto expected = datagram.subspan(*integrity_offset + AttributeHeaderSize, IntegritySize);
    return CRYPTO_memcmp(digest.data(), expected.data(), IntegritySize) == 0;
}

}  // namespace endgate::rtc
