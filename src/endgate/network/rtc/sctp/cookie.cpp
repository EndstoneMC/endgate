#include "endgate/network/rtc/sctp/cookie.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "endgate/network/rtc/sctp/packet.h"

namespace endgate::rtc::sctp {
namespace {

constexpr std::size_t fields_size = 41;
constexpr std::size_t mac_size = 32;

std::array<std::byte, mac_size> authenticate(const StateCookie::Key &key, std::span<const std::byte> fields)
{
    std::array<std::byte, mac_size> mac{};
    unsigned int length = mac.size();
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(fields.data()),
         fields.size(), reinterpret_cast<unsigned char *>(mac.data()), &length);
    return mac;
}

std::uint64_t load(std::span<const std::byte> data, std::size_t offset, std::size_t size)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value = value << 8U | std::to_integer<std::uint64_t>(data[offset + i]);
    }
    return value;
}

}  // namespace

std::vector<std::byte> StateCookie::seal(const Key &key, std::chrono::steady_clock::time_point now) const
{
    std::vector<std::byte> data;
    data.reserve(fields_size + mac_size);
    ByteWriter writer(data);
    writer.write64(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()));
    writer.write32(local_tag);
    writer.write32(peer_tag);
    writer.write32(local_initial_tsn);
    writer.write32(peer_initial_tsn);
    writer.write32(peer_a_rwnd);
    writer.write16(peer_outbound_streams);
    writer.write16(peer_inbound_streams);
    writer.write64(tie_tag);
    writer.write8(
        static_cast<std::uint8_t>((peer_supports_forward_tsn ? 1U : 0U) | (peer_supports_reconfig ? 2U : 0U)));
    writer.write(authenticate(key, data));
    return data;
}

std::optional<StateCookie::Opened> StateCookie::open(const Key &key, std::span<const std::byte> data,
                                                     std::chrono::steady_clock::time_point now,
                                                     std::chrono::steady_clock::duration lifetime)
{
    if (data.size() != fields_size + mac_size) {
        return std::nullopt;
    }
    const auto mac = authenticate(key, data.first(fields_size));
    if (CRYPTO_memcmp(mac.data(), data.subspan(fields_size).data(), mac_size) != 0) {
        return std::nullopt;
    }
    const auto created =
        std::chrono::steady_clock::time_point{std::chrono::milliseconds{static_cast<std::int64_t>(load(data, 0, 8))}};
    if (created > now) {
        return std::nullopt;
    }
    const auto flags = load(data, 40, 1);
    return Opened{
        .cookie =
            {
                .local_tag = static_cast<std::uint32_t>(load(data, 8, 4)),
                .peer_tag = static_cast<std::uint32_t>(load(data, 12, 4)),
                .local_initial_tsn = static_cast<std::uint32_t>(load(data, 16, 4)),
                .peer_initial_tsn = static_cast<std::uint32_t>(load(data, 20, 4)),
                .peer_a_rwnd = static_cast<std::uint32_t>(load(data, 24, 4)),
                .peer_outbound_streams = static_cast<std::uint16_t>(load(data, 28, 2)),
                .peer_inbound_streams = static_cast<std::uint16_t>(load(data, 30, 2)),
                .tie_tag = load(data, 32, 8),
                .peer_supports_forward_tsn = (flags & 1U) != 0,
                .peer_supports_reconfig = (flags & 2U) != 0,
            },
        .age = now - created,
        .expired = now - created > lifetime,
    };
}

}  // namespace endgate::rtc::sctp
