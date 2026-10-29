#include "endgate/crypto/encryption.h"

#include <memory>
#include <vector>

namespace endgate {

std::optional<std::array<std::byte, 32>> deriveEncryptionKey(EVP_PKEY *own, EVP_PKEY *peer,
                                                             std::span<const std::byte> salt)
{
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> derive(EVP_PKEY_CTX_new(own, nullptr),
                                                                             EVP_PKEY_CTX_free);
    std::size_t length = 0;
    if (!derive || EVP_PKEY_derive_init(derive.get()) != 1 || EVP_PKEY_derive_set_peer(derive.get(), peer) != 1 ||
        EVP_PKEY_derive(derive.get(), nullptr, &length) != 1) {
        return std::nullopt;
    }
    std::vector<unsigned char> secret(length);
    if (EVP_PKEY_derive(derive.get(), secret.data(), &length) != 1) {
        return std::nullopt;
    }
    secret.resize(length);

    std::array<std::byte, 32> key{};
    unsigned int written = 0;
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(hash.get(), salt.data(), salt.size()) != 1 ||
        EVP_DigestUpdate(hash.get(), secret.data(), secret.size()) != 1 ||
        EVP_DigestFinal_ex(hash.get(), reinterpret_cast<unsigned char *>(key.data()), &written) != 1 ||
        written != key.size()) {
        return std::nullopt;
    }
    return key;
}

}  // namespace endgate
