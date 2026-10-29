#include "endgate/network/rtc/sctp/cookie.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace endgate::rtc::sctp {
namespace {

constexpr std::size_t FIELDS_SIZE = 41;
constexpr std::size_t MAC_SIZE = 32;

std::array<std::byte, MAC_SIZE> authenticate(const StateCookie::Key &key, std::span<const std::byte> fields)
{
    std::array<std::byte, MAC_SIZE> mac{};
    unsigned int length = mac.size();
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), reinterpret_cast<const unsigned char *>(fields.data()),
         fields.size(), reinterpret_cast<unsigned char *>(mac.data()), &length);
    return mac;
}

}  // namespace

ByteBuf StateCookie::seal(const Key &key, std::chrono::steady_clock::time_point now) const
{
    auto data = Unpooled::buffer(FIELDS_SIZE + MAC_SIZE);
    data.writeLong(static_cast<std::uint64_t>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()))
        .writeInt(local_tag)
        .writeInt(peer_tag)
        .writeInt(local_initial_tsn)
        .writeInt(peer_initial_tsn)
        .writeInt(peer_a_rwnd)
        .writeShort(peer_outbound_streams)
        .writeShort(peer_inbound_streams)
        .writeLong(tie_tag)
        .writeByte(
            static_cast<std::uint8_t>((peer_supports_forward_tsn ? 1U : 0U) | (peer_supports_reconfig ? 2U : 0U)));
    data.writeBytes(authenticate(key, data.nioBuffer()));
    return data;
}

std::optional<StateCookie::Opened> StateCookie::open(const Key &key, ByteBufView data,
                                                     std::chrono::steady_clock::time_point now,
                                                     std::chrono::steady_clock::duration lifetime)
{
    if (data.readableBytes() != FIELDS_SIZE + MAC_SIZE) {
        return std::nullopt;
    }
    const auto bytes = data.nioBuffer();
    const auto mac = authenticate(key, bytes.first(FIELDS_SIZE));
    if (CRYPTO_memcmp(mac.data(), bytes.subspan(FIELDS_SIZE).data(), MAC_SIZE) != 0) {
        return std::nullopt;
    }
    const auto created = std::chrono::steady_clock::time_point{std::chrono::milliseconds{data.readLong()}};
    if (created > now) {
        return std::nullopt;
    }
    const auto local_tag = data.readUnsignedInt();
    const auto peer_tag = data.readUnsignedInt();
    const auto local_initial_tsn = data.readUnsignedInt();
    const auto peer_initial_tsn = data.readUnsignedInt();
    const auto peer_a_rwnd = data.readUnsignedInt();
    const auto peer_outbound_streams = data.readUnsignedShort();
    const auto peer_inbound_streams = data.readUnsignedShort();
    const auto tie_tag = static_cast<std::uint64_t>(data.readLong());
    const auto flags = data.readUnsignedByte();
    return Opened{
        .cookie =
            {
                .local_tag = local_tag,
                .peer_tag = peer_tag,
                .local_initial_tsn = local_initial_tsn,
                .peer_initial_tsn = peer_initial_tsn,
                .peer_a_rwnd = peer_a_rwnd,
                .peer_outbound_streams = peer_outbound_streams,
                .peer_inbound_streams = peer_inbound_streams,
                .tie_tag = tie_tag,
                .peer_supports_forward_tsn = (flags & 1U) != 0,
                .peer_supports_reconfig = (flags & 2U) != 0,
            },
        .age = now - created,
        .expired = now - created > lifetime,
    };
}

}  // namespace endgate::rtc::sctp
