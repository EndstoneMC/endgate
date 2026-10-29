#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <openssl/evp.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/codec.h"

namespace endgate {

/**
 * One direction of a connection's batch encryption: AES-256-GCM run as a keystream across every batch
 * in turn, never finalised, with the first 8 bytes of SHA-256(counter, batch, key) sealed in behind
 * each batch. The key is SHA-256 of the handshake's salt and the ECDH secret; its first bytes are the
 * IV too.
 *
 * @see com.velocitypowered.natives.encryption.VelocityCipher
 * @see EncryptedNetworkPeer
 */
class MinecraftCipher {
public:
    static constexpr std::size_t KEY_SIZE = 32;
    static constexpr std::size_t CHECKSUM_SIZE = 8;

    enum class Mode : std::uint8_t {
        Encrypt,
        Decrypt,
    };

    MinecraftCipher(std::span<const std::byte, KEY_SIZE> key, Mode mode);

    /**
     * Appends the batch's checksum, then encrypts the batch and the checksum in place.
     *
     * @see EncryptedNetworkPeer::sendPacket
     */
    void seal(ByteBuf &batch);

    /**
     * Decrypts the batch, checks its checksum and returns it without the checksum.
     *
     * @see EncryptedNetworkPeer::_receivePacket
     */
    [[nodiscard]] ByteBuf open(const ByteBufView &batch);

private:
    [[nodiscard]] std::array<std::byte, CHECKSUM_SIZE> checksum(std::span<const std::byte> batch) const;

    std::array<std::byte, KEY_SIZE> key_;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context_;
    std::uint64_t counter_{0};
};

/**
 * Decrypts what a connection receives once login turns encryption on. Until then it passes batches on.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftCipherDecoder
 * @see EncryptedNetworkPeer
 */
class MinecraftCipherDecoder : public MessageToMessageDecoder {
public:
    void enableEncryption(std::span<const std::byte, MinecraftCipher::KEY_SIZE> key)
    {
        cipher_.emplace(key, MinecraftCipher::Mode::Decrypt);
    }

    template <typename Context>
    void decode(Context &ctx, ByteBufView batch)
    {
        if (!cipher_) {
            ctx.fireChannelRead(batch);
            return;
        }
        const auto decrypted = cipher_->open(batch);
        ctx.fireChannelRead(decrypted.slice());
    }

private:
    std::optional<MinecraftCipher> cipher_;
};

/**
 * Encrypts what a connection sends once login turns encryption on. Until then it passes batches on.
 *
 * @see com.velocitypowered.proxy.protocol.netty.MinecraftCipherEncoder
 * @see EncryptedNetworkPeer
 */
class MinecraftCipherEncoder : public ChannelOutboundHandlerAdapter {
public:
    void enableEncryption(std::span<const std::byte, MinecraftCipher::KEY_SIZE> key)
    {
        cipher_.emplace(key, MinecraftCipher::Mode::Encrypt);
    }

    template <typename Context>
    void write(Context &ctx, ByteBuf batch)
    {
        if (cipher_) {
            cipher_->seal(batch);
        }
        ctx.write(std::move(batch));
    }

private:
    std::optional<MinecraftCipher> cipher_;
};

}  // namespace endgate
