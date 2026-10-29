#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

namespace endgate {

using PublicKey = std::shared_ptr<EVP_PKEY>;
using PrivateKey = std::shared_ptr<EVP_PKEY>;

/**
 * @see RFC 4648 section 4
 */
[[nodiscard]] std::string base64Encode(std::span<const std::byte> bytes);

/**
 * @see RFC 4648 section 4
 */
[[nodiscard]] std::optional<std::vector<std::byte>> base64Decode(std::string_view text);

/**
 * @see RFC 4648 section 5
 */
[[nodiscard]] std::string base64UrlEncode(std::span<const std::byte> bytes);

/**
 * @see RFC 4648 section 5
 */
[[nodiscard]] std::optional<std::vector<std::byte>> base64UrlDecode(std::string_view text);

/**
 * @see RFC 7518 section 3.4
 * @see RFC 7518 section 6.2.1.1
 */
struct EllipticCurve {
    std::string_view algorithm;
    std::string_view jwk_name;
    std::string_view group_name;
    int bits;
    std::size_t coordinate_size;
    const EVP_MD *(*digest)();
};

[[nodiscard]] const EllipticCurve *curveOf(EVP_PKEY *key);

/**
 * Signs a JWS signing input with an EC key, as the curve's ES algorithm does.
 *
 * @see RFC 7518 section 3.4
 */
[[nodiscard]] std::optional<std::string> signJws(EVP_PKEY *key, std::string_view signing_input);

/**
 * Reads the public key a JSON Web Key holds: RSA, or EC on P-256, P-384 or P-521.
 *
 * @see RFC 7517 section 4
 * @see RFC 7518 section 6
 */
[[nodiscard]] PublicKey decodeJsonWebKey(std::string_view json);

[[nodiscard]] PublicKey decodeSubjectPublicKeyInfo(std::string_view base64);

/**
 * A compact JSON Web Token split into its parts, signature unchecked until verify is called.
 *
 * @see RFC 7519
 * @see RFC 7515 section 7.1
 */
struct JsonWebToken {
    std::string header;
    std::string claims;
    std::string algorithm;
    std::string key_id;
    std::string signing_input;
    std::vector<std::byte> signature;

    [[nodiscard]] static std::optional<JsonWebToken> decode(std::string_view token);

    /**
     * @see RFC 7518 section 3.3
     * @see RFC 7518 section 3.4
     */
    [[nodiscard]] bool verify(EVP_PKEY *key) const;

    [[nodiscard]] PublicKey publicKey() const;
};

/**
 * @see RFC 7515 appendix F
 */
[[nodiscard]] bool verifyDetachedJws(std::string_view jws, std::string_view payload, EVP_PKEY *key);

}  // namespace endgate
