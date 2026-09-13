#include "endgate/network/discovery.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <random>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "endgate/config.h"
#include "endgate/network/cursor.h"
#include "endgate/network/ping.h"
#include "endgate/server.h"

namespace endgate {

namespace {

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

/**
 * @see NetherNet::DiscoveryPacketType
 */
enum class DiscoveryPacketType : std::uint16_t {
    Request = 0,
    Response = 1,
    Message = 2,
};

#pragma pack(push, 1)

/**
 * @see NetherNet::DiscoveryPacket
 */
struct DiscoveryPacket {
    std::uint16_t packet_length;
    std::uint16_t packet_type;
    std::uint64_t sender_id;
    std::array<char, 8> pad;
};

/**
 * @see NetherNet::DiscoveryResponsePacketHeader
 */
struct DiscoveryResponsePacketHeader : DiscoveryPacket {
    std::uint32_t application_data_length;
};

#pragma pack(pop)

static_assert(sizeof(DiscoveryPacket) == 20);
static_assert(sizeof(DiscoveryResponsePacketHeader) == 24);

}  // namespace

LanDiscoveryHandler::LanDiscoveryHandler(ProxyServer &server) : server_(server)
{
    constexpr std::uint64_t application_unique_id = 0xDEADBEEF;

    std::random_device entropy;
    std::mt19937_64 generator(entropy());
    network_id_ = generator();

    key_.resize(SHA256_DIGEST_LENGTH);
    SHA256(reinterpret_cast<const unsigned char *>(&application_unique_id), sizeof(application_unique_id), key_.data());
}

ByteBuf LanDiscoveryHandler::handle(std::span<const std::byte> datagram) const
{
    constexpr std::uint32_t legacy_payload_max_size = 1148;

    const auto plaintext = open(datagram);
    if (plaintext.size() < sizeof(DiscoveryPacket)) {
        return {};
    }

    DiscoveryPacket request{};
    std::memcpy(&request, plaintext.data(), sizeof(request));
    if (request.packet_length > plaintext.size() ||
        request.packet_type != static_cast<std::uint16_t>(DiscoveryPacketType::Request)) {
        return {};
    }

    const auto &application_data = cached_broadcast_discovery_response_;

    DiscoveryResponsePacketHeader header{};
    header.packet_length = static_cast<std::uint16_t>(sizeof(header) + application_data.size());
    header.packet_type = static_cast<std::uint16_t>(DiscoveryPacketType::Response);
    header.sender_id = network_id_;
    header.application_data_length =
        std::min(static_cast<std::uint32_t>(application_data.size()), legacy_payload_max_size);

    std::vector<std::byte> response(sizeof(header) + application_data.size());
    std::memcpy(response.data(), &header, sizeof(header));
    std::memcpy(response.data() + sizeof(header), application_data.data(), application_data.size());

    return ByteBuf::copyBuffer(seal(response));
}

void LanDiscoveryHandler::cacheDiscoveryResponseData()
{
    constexpr std::uint8_t version_number = 6;
    constexpr bool editor_world = false;
    constexpr bool hardcore = false;
    constexpr std::int32_t nether_web_sockets = 2;
    constexpr std::int32_t lan_web_rtc_signaling = 4;
    constexpr std::string_view digits = "0123456789abcdef";
    constexpr std::size_t server_data_growth = 512;

    const auto ping = server_.getServerListPingHandler().getInitialPing();
    const bool online_mode = server_.getConfiguration().isOnlineMode();

    ByteBuf data;
    Appender appender(data, server_data_growth);
    appender.writeByte(version_number);
    appender.writeString(ping.getDescription());
    appender.writeString(ping.getLevelName());
    appender.writeVarInt(static_cast<std::int32_t>(ping.getGameType()));
    appender.writeSignedIntLE(ping.getPlayers().getOnline());
    appender.writeSignedIntLE(ping.getPlayers().getMax());
    appender.writeBool(editor_world);
    appender.writeBool(hardcore);
    appender.writeBool(online_mode);
    appender.writeBool(!online_mode);
    appender.writeString({});
    appender.writeVarInt(nether_web_sockets);
    appender.writeVarInt(lan_web_rtc_signaling);

    std::vector<std::byte> raw(data.length());
    data.copyTo(raw);

    cached_broadcast_discovery_response_.clear();
    cached_broadcast_discovery_response_.reserve(raw.size() * 2);
    for (const auto value : raw) {
        const auto byte = std::to_integer<std::uint8_t>(value);
        cached_broadcast_discovery_response_.push_back(digits[byte >> 4U]);
        cached_broadcast_discovery_response_.push_back(digits[byte & 0x0FU]);
    }
}

std::vector<std::byte> LanDiscoveryHandler::seal(std::span<const std::byte> plaintext) const
{
    const EVP_CIPHER *cipher = EVP_aes_256_ecb();
    const CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context || EVP_EncryptInit_ex(context.get(), cipher, nullptr, key_.data(), key_.data()) != 1) {
        return {};
    }

    std::vector<std::byte> envelope(SHA256_DIGEST_LENGTH + plaintext.size() + 2 * EVP_CIPHER_block_size(cipher));
    auto *out = reinterpret_cast<unsigned char *>(envelope.data());
    const auto *in = reinterpret_cast<const unsigned char *>(plaintext.data());

    if (HMAC(EVP_sha256(), key_.data(), static_cast<int>(key_.size()), in, plaintext.size(), out, nullptr) == nullptr) {
        return {};
    }

    int encrypted = 0;
    int finalised = 0;
    if (EVP_EncryptUpdate(context.get(), out + SHA256_DIGEST_LENGTH, &encrypted, in,
                          static_cast<int>(plaintext.size())) != 1 ||
        EVP_EncryptFinal_ex(context.get(), out + SHA256_DIGEST_LENGTH + encrypted, &finalised) != 1) {
        return {};
    }

    envelope.resize(SHA256_DIGEST_LENGTH + encrypted + finalised);
    return envelope;
}

std::vector<std::byte> LanDiscoveryHandler::open(std::span<const std::byte> envelope) const
{
    if (envelope.size() <= SHA256_DIGEST_LENGTH) {
        return {};
    }

    const CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_256_ecb(), nullptr, key_.data(), key_.data()) != 1) {
        return {};
    }

    std::vector<std::byte> plaintext(envelope.size());
    const auto *in = reinterpret_cast<const unsigned char *>(envelope.data());
    auto *out = reinterpret_cast<unsigned char *>(plaintext.data());

    int decrypted = 0;
    int finalised = 0;
    if (EVP_DecryptUpdate(context.get(), out, &decrypted, in + SHA256_DIGEST_LENGTH,
                          static_cast<int>(envelope.size() - SHA256_DIGEST_LENGTH)) != 1 ||
        EVP_DecryptFinal_ex(context.get(), out + decrypted, &finalised) != 1) {
        return {};
    }
    plaintext.resize(decrypted + finalised);

    std::array<unsigned char, SHA256_DIGEST_LENGTH> mac{};
    if (HMAC(EVP_sha256(), key_.data(), static_cast<int>(key_.size()), out, plaintext.size(), mac.data(), nullptr) ==
            nullptr ||
        CRYPTO_memcmp(in, mac.data(), mac.size()) != 0) {
        return {};
    }
    return plaintext;
}

}  // namespace endgate
