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

#include "endgate/buffer/bytebuf.h"

namespace endgate::rtc {

namespace {

constexpr std::uint32_t MAGIC_COOKIE = 0x2112A442;
constexpr std::size_t HEADER_SIZE = 20;
constexpr std::size_t ATTRIBUTE_HEADER_SIZE = 4;
constexpr std::size_t INTEGRITY_SIZE = 20;
constexpr std::size_t MAX_USERNAME_LENGTH = 513;
constexpr std::size_t MAX_TEXT_LENGTH = 763;
constexpr std::size_t MAX_UNKNOWN_ATTRIBUTES = 16;

std::array<std::byte, 16> xorMask(const StunMessage::TransactionId &transaction_id)
{
    std::array<std::byte, 16> mask{};
    mask[0] = static_cast<std::byte>(MAGIC_COOKIE >> 24U);
    mask[1] = static_cast<std::byte>(MAGIC_COOKIE >> 16U);
    mask[2] = static_cast<std::byte>(MAGIC_COOKIE >> 8U);
    mask[3] = static_cast<std::byte>(MAGIC_COOKIE);
    std::ranges::copy(transaction_id, mask.begin() + 4);
    return mask;
}

std::uint32_t fingerprintOf(std::span<const std::byte> bytes)
{
    return static_cast<std::uint32_t>(libdeflate_crc32(0, bytes.data(), bytes.size())) ^ 0x5354554EU;
}

std::array<std::byte, INTEGRITY_SIZE> hmacSha1(std::span<const std::byte> header, std::span<const std::byte> body,
                                               std::string_view key)
{
    static EVP_MAC *const mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    std::array<std::byte, INTEGRITY_SIZE> digest{};
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

std::optional<asio::ip::udp::endpoint> readXorAddress(const ByteBufView &value,
                                                      const StunMessage::TransactionId &transaction_id)
{
    const auto mask = xorMask(transaction_id);
    const auto family = value.getUnsignedByte(1);
    const auto port = static_cast<std::uint16_t>(value.getUnsignedShort(2) ^ (MAGIC_COOKIE >> 16U));
    if (family == 0x01 && value.readableBytes() == 8) {
        asio::ip::address_v4::bytes_type bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] =
                static_cast<unsigned char>(value.getUnsignedByte(4 + i) ^ std::to_integer<unsigned char>(mask[i]));
        }
        return asio::ip::udp::endpoint{asio::ip::address_v4{bytes}, port};
    }
    if (family == 0x02 && value.readableBytes() == 20) {
        asio::ip::address_v6::bytes_type bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] =
                static_cast<unsigned char>(value.getUnsignedByte(4 + i) ^ std::to_integer<unsigned char>(mask[i]));
        }
        return asio::ip::udp::endpoint{asio::ip::address_v6{bytes}, port};
    }
    return std::nullopt;
}

bool isHeaderValid(const ByteBufView &datagram)
{
    if (!datagram.isReadable(HEADER_SIZE) || (datagram.getUnsignedByte(0) & 0xC0U) != 0) {
        return false;
    }
    const auto length = datagram.getUnsignedShort(2);
    return datagram.getUnsignedInt(4) == MAGIC_COOKIE && length % 4 == 0 &&
           datagram.readableBytes() == HEADER_SIZE + length;
}

}  // namespace

std::optional<StunMessage> StunMessage::decode(ByteBufView datagram)
{
    datagram = datagram.slice();
    if (!isHeaderValid(datagram)) {
        return std::nullopt;
    }

    StunMessage message;
    const auto type = datagram.getUnsignedShort(0);
    message.message_class = static_cast<StunClass>(type & 0x0110U);
    message.method = static_cast<StunMethod>((type & 0x000FU) | ((type & 0x00E0U) >> 1U) | ((type & 0x3E00U) >> 2U));
    datagram.getBytes(8, message.transaction_id);

    bool after_integrity = false;
    bool after_integrity_sha256 = false;
    std::size_t offset = HEADER_SIZE;
    while (offset < datagram.readableBytes()) {
        if (message.fingerprint || datagram.readableBytes() - offset < ATTRIBUTE_HEADER_SIZE) {
            return std::nullopt;
        }
        const auto attribute = datagram.getUnsignedShort(offset);
        const std::size_t length = datagram.getUnsignedShort(offset + 2);
        const auto padded = (length + 3) & ~std::size_t{3};
        if (datagram.readableBytes() - offset - ATTRIBUTE_HEADER_SIZE < padded) {
            return std::nullopt;
        }
        const auto value = datagram.slice(offset + ATTRIBUTE_HEADER_SIZE, length);
        const auto attribute_offset = offset;
        offset += ATTRIBUTE_HEADER_SIZE + padded;

        if (attribute == static_cast<std::uint16_t>(StunAttribute::Fingerprint)) {
            if (length != 4 || value.getUnsignedInt(0) != fingerprintOf(datagram.nioBuffer(0, attribute_offset))) {
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
            if (length > MAX_USERNAME_LENGTH) {
                return std::nullopt;
            }
            if (!message.username) {
                message.username.emplace(reinterpret_cast<const char *>(value.nioBuffer().data()), length);
            }
            break;
        case StunAttribute::MessageIntegrity:
            if (length != INTEGRITY_SIZE) {
                return std::nullopt;
            }
            message.integrity_offset = attribute_offset;
            after_integrity = true;
            break;
        case StunAttribute::ErrorCode:
            if (length < 4 || length - 4 > MAX_TEXT_LENGTH) {
                return std::nullopt;
            }
            if (!message.error_code) {
                message.error_code = StunErrorCode{
                    .code = static_cast<std::uint16_t>((value.getUnsignedByte(2) & 0x07U) * 100U +
                                                       value.getUnsignedByte(3)),
                    .reason = std::string(reinterpret_cast<const char *>(value.nioBuffer().data()) + 4, length - 4)};
            }
            break;
        case StunAttribute::UnknownAttributes:
            if (length % 2 != 0) {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < length && message.unknown_attributes.size() < MAX_UNKNOWN_ATTRIBUTES; i += 2) {
                message.unknown_attributes.push_back(value.getUnsignedShort(i));
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
                message.priority = value.getUnsignedInt(0);
            }
            break;
        case StunAttribute::UseCandidate:
            if (length != 0) {
                return std::nullopt;
            }
            message.use_candidate = true;
            break;
        case StunAttribute::Software:
            if (length > MAX_TEXT_LENGTH) {
                return std::nullopt;
            }
            if (!message.software) {
                message.software.emplace(reinterpret_cast<const char *>(value.nioBuffer().data()), length);
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
                tiebreaker = static_cast<std::uint64_t>(value.getLong(0));
            }
            break;
        }
        default:
            if (attribute < 0x8000 && message.unknown_comprehension_required.size() < MAX_UNKNOWN_ATTRIBUTES &&
                !std::ranges::contains(message.unknown_comprehension_required, attribute)) {
                message.unknown_comprehension_required.push_back(attribute);
            }
            break;
        }
    }
    return message;
}

std::optional<std::string_view> StunMessage::peekLocalUfrag(ByteBufView datagram)
{
    datagram = datagram.slice();
    if (!isHeaderValid(datagram) || datagram.getUnsignedShort(0) != static_cast<std::uint16_t>(StunMethod::Binding)) {
        return std::nullopt;
    }
    std::size_t offset = HEADER_SIZE;
    while (datagram.readableBytes() - offset >= ATTRIBUTE_HEADER_SIZE) {
        const auto attribute = datagram.getUnsignedShort(offset);
        const std::size_t length = datagram.getUnsignedShort(offset + 2);
        const auto padded = (length + 3) & ~std::size_t{3};
        if (datagram.readableBytes() - offset - ATTRIBUTE_HEADER_SIZE < padded ||
            attribute == static_cast<std::uint16_t>(StunAttribute::MessageIntegrity)) {
            return std::nullopt;
        }
        if (attribute == static_cast<std::uint16_t>(StunAttribute::Username)) {
            if (length > MAX_USERNAME_LENGTH) {
                return std::nullopt;
            }
            const std::string_view username(
                reinterpret_cast<const char *>(datagram.nioBuffer(offset + ATTRIBUTE_HEADER_SIZE, length).data()),
                length);
            const auto colon = username.find(':');
            if (colon == std::string_view::npos || colon == 0) {
                return std::nullopt;
            }
            return username.substr(0, colon);
        }
        offset += ATTRIBUTE_HEADER_SIZE + padded;
    }
    return std::nullopt;
}

ByteBuf StunMessage::encode(std::optional<std::string_view> integrity_key) const
{
    auto out = Unpooled::buffer(128);
    const auto method_bits = static_cast<std::uint16_t>(method);
    out.writeShort(static_cast<std::uint16_t>((method_bits & 0x000FU) | ((method_bits & 0x0070U) << 1U) |
                                              ((method_bits & 0x0F80U) << 2U) |
                                              static_cast<std::uint16_t>(message_class)));
    out.writeShort(0);
    out.writeInt(MAGIC_COOKIE);
    out.writeBytes(transaction_id);

    if (xor_mapped_address) {
        const auto mask = xorMask(transaction_id);
        const auto address = xor_mapped_address->address();
        out.writeShort(static_cast<std::uint16_t>(StunAttribute::XorMappedAddress));
        out.writeShort(address.is_v4() ? 8 : 20);
        out.writeByte(0);
        out.writeByte(address.is_v4() ? 0x01 : 0x02);
        out.writeShort(static_cast<std::uint16_t>(xor_mapped_address->port() ^ (MAGIC_COOKIE >> 16U)));
        if (address.is_v4()) {
            const auto bytes = address.to_v4().to_bytes();
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                out.writeByte(static_cast<std::uint8_t>(bytes[i] ^ std::to_integer<unsigned char>(mask[i])));
            }
        }
        else {
            const auto bytes = address.to_v6().to_bytes();
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                out.writeByte(static_cast<std::uint8_t>(bytes[i] ^ std::to_integer<unsigned char>(mask[i])));
            }
        }
    }
    if (error_code) {
        const auto reason =
            std::as_bytes(std::span{error_code->reason.data(), std::min(error_code->reason.size(), MAX_TEXT_LENGTH)});
        out.writeShort(static_cast<std::uint16_t>(StunAttribute::ErrorCode));
        out.writeShort(static_cast<std::uint16_t>(4 + reason.size()));
        out.writeShort(0);
        out.writeByte(static_cast<std::uint8_t>(error_code->code / 100));
        out.writeByte(static_cast<std::uint8_t>(error_code->code % 100));
        out.writeBytes(reason);
        out.writeZero((4 - reason.size() % 4) % 4);
    }
    if (!unknown_attributes.empty()) {
        const auto count = std::min(unknown_attributes.size(), MAX_UNKNOWN_ATTRIBUTES);
        out.writeShort(static_cast<std::uint16_t>(StunAttribute::UnknownAttributes));
        out.writeShort(static_cast<std::uint16_t>(count * 2));
        for (const auto attribute : unknown_attributes | std::views::take(MAX_UNKNOWN_ATTRIBUTES)) {
            out.writeShort(attribute);
        }
        out.writeZero((4 - count * 2 % 4) % 4);
    }
    if (username) {
        const auto text = std::as_bytes(std::span{username->data(), std::min(username->size(), MAX_USERNAME_LENGTH)});
        out.writeShort(static_cast<std::uint16_t>(StunAttribute::Username));
        out.writeShort(static_cast<std::uint16_t>(text.size()));
        out.writeBytes(text);
        out.writeZero((4 - text.size() % 4) % 4);
    }
    if (priority) {
        out.writeShort(static_cast<std::uint16_t>(StunAttribute::Priority));
        out.writeShort(4);
        out.writeInt(*priority);
    }
    if (use_candidate) {
        out.writeShort(static_cast<std::uint16_t>(StunAttribute::UseCandidate));
        out.writeShort(0);
    }
    if (ice_controlled) {
        out.writeShort(static_cast<std::uint16_t>(StunAttribute::IceControlled));
        out.writeShort(8);
        out.writeLong(*ice_controlled);
    }
    if (ice_controlling) {
        out.writeShort(static_cast<std::uint16_t>(StunAttribute::IceControlling));
        out.writeShort(8);
        out.writeLong(*ice_controlling);
    }
    if (software) {
        const auto text = std::as_bytes(std::span{software->data(), std::min(software->size(), MAX_TEXT_LENGTH)});
        out.writeShort(static_cast<std::uint16_t>(StunAttribute::Software));
        out.writeShort(static_cast<std::uint16_t>(text.size()));
        out.writeBytes(text);
        out.writeZero((4 - text.size() % 4) % 4);
    }

    if (integrity_key) {
        out.setShort(
            2, static_cast<std::uint16_t>(out.writerIndex() - HEADER_SIZE + ATTRIBUTE_HEADER_SIZE + INTEGRITY_SIZE));
        const auto bytes = out.nioBuffer();
        const auto digest = hmacSha1(bytes.first(HEADER_SIZE), bytes.subspan(HEADER_SIZE), *integrity_key);
        out.writeShort(static_cast<std::uint16_t>(StunAttribute::MessageIntegrity));
        out.writeShort(INTEGRITY_SIZE);
        out.writeBytes(digest);
    }

    out.setShort(2, static_cast<std::uint16_t>(out.writerIndex() - HEADER_SIZE + ATTRIBUTE_HEADER_SIZE + 4));
    const auto fingerprint = fingerprintOf(out.nioBuffer());
    out.writeShort(static_cast<std::uint16_t>(StunAttribute::Fingerprint));
    out.writeShort(4);
    out.writeInt(fingerprint);
    return out;
}

bool StunMessage::verifyIntegrity(ByteBufView datagram, std::string_view key) const
{
    datagram = datagram.slice();
    if (!integrity_offset || *integrity_offset + ATTRIBUTE_HEADER_SIZE + INTEGRITY_SIZE > datagram.readableBytes()) {
        return false;
    }
    std::array<std::byte, HEADER_SIZE> header{};
    datagram.getBytes(0, header);
    const auto length = *integrity_offset - HEADER_SIZE + ATTRIBUTE_HEADER_SIZE + INTEGRITY_SIZE;
    header[2] = static_cast<std::byte>(length >> 8U);
    header[3] = static_cast<std::byte>(length);
    const auto digest = hmacSha1(header, datagram.nioBuffer(HEADER_SIZE, *integrity_offset - HEADER_SIZE), key);
    const auto expected = datagram.nioBuffer(*integrity_offset + ATTRIBUTE_HEADER_SIZE, INTEGRITY_SIZE);
    return CRYPTO_memcmp(digest.data(), expected.data(), INTEGRITY_SIZE) == 0;
}

}  // namespace endgate::rtc
