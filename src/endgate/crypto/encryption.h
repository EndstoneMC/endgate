#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>

#include <openssl/evp.h>

namespace endgate {

/**
 * Derives a connection's batch cipher key from its handshake: SHA-256 of the salt followed by the ECDH
 * secret of one side's private key and the other side's public key.
 *
 * @see com.velocitypowered.proxy.crypto.EncryptionUtils
 * @see ServerNetworkHandler::_onClientAsyncAuthorized
 */
[[nodiscard]] std::optional<std::array<std::byte, 32>> deriveEncryptionKey(EVP_PKEY *own, EVP_PKEY *peer,
                                                                           std::span<const std::byte> salt);

}  // namespace endgate
