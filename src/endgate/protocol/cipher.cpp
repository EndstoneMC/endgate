#include "endgate/protocol/cipher.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace endgate {

MinecraftCipher::MinecraftCipher(std::span<const std::byte, KEY_SIZE> key, Mode mode)
    : context_(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free)
{
    std::ranges::copy(key, key_.begin());
    const auto *const raw = reinterpret_cast<const unsigned char *>(key_.data());
    if (!context_) {
        throw std::runtime_error("unable to create the batch cipher");
    }
    const auto initialised = mode == Mode::Encrypt
                               ? EVP_EncryptInit_ex(context_.get(), EVP_aes_256_gcm(), nullptr, raw, raw)
                               : EVP_DecryptInit_ex(context_.get(), EVP_aes_256_gcm(), nullptr, raw, raw);
    if (initialised != 1) {
        throw std::runtime_error("unable to create the batch cipher");
    }
}

void MinecraftCipher::seal(ByteBuf &batch)
{
    const auto sum = checksum(batch.nioBuffer());
    batch.writeBytes(sum);
    const auto bytes = batch.nioBuffer();
    auto *const data = reinterpret_cast<unsigned char *>(bytes.data());
    const auto size = static_cast<int>(bytes.size());
    int produced = 0;
    if (EVP_EncryptUpdate(context_.get(), data, &produced, data, size) != 1 || produced != size) {
        throw EncoderException("batch could not be encrypted");
    }
    ++counter_;
}

ByteBuf MinecraftCipher::open(const ByteBufView &batch)
{
    const auto input = batch.nioBuffer();
    if (input.size() < CHECKSUM_SIZE) {
        throw DecoderException("batch is too short to carry its checksum");
    }
    auto out = Unpooled::buffer(input.size());
    const auto writable = out.nioBuffer(0, input.size());
    const auto size = static_cast<int>(input.size());
    int produced = 0;
    if (EVP_DecryptUpdate(context_.get(), reinterpret_cast<unsigned char *>(writable.data()), &produced,
                          reinterpret_cast<const unsigned char *>(input.data()), size) != 1 ||
        produced != size) {
        throw DecoderException("batch could not be decrypted");
    }
    const auto plaintext = writable.first(input.size() - CHECKSUM_SIZE);
    if (!std::ranges::equal(checksum(plaintext), writable.last(CHECKSUM_SIZE))) {
        throw DecoderException("batch checksum does not match");
    }
    ++counter_;
    out.writerIndex(plaintext.size());
    return out;
}

std::array<std::byte, MinecraftCipher::CHECKSUM_SIZE> MinecraftCipher::checksum(std::span<const std::byte> batch) const
{
    std::array<unsigned char, sizeof(counter_)> counter{};
    for (std::size_t i = 0; i < counter.size(); ++i) {
        counter.at(i) = static_cast<unsigned char>(counter_ >> (8 * i));
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), counter.data(), counter.size()) != 1 ||
        EVP_DigestUpdate(context.get(), batch.data(), batch.size()) != 1 ||
        EVP_DigestUpdate(context.get(), key_.data(), key_.size()) != 1 ||
        EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1) {
        throw std::runtime_error("batch checksum could not be computed");
    }
    std::array<std::byte, CHECKSUM_SIZE> sum{};
    std::memcpy(sum.data(), digest.data(), sum.size());
    return sum;
}

}  // namespace endgate
