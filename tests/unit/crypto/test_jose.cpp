#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include "endgate/crypto/jose.h"

namespace endgate {
namespace {

PrivateKey generateKey(const char *curve)
{
    return {EVP_EC_gen(curve), EVP_PKEY_free};
}

std::string base64UrlOf(const BIGNUM *number)
{
    std::string bytes(static_cast<std::size_t>(BN_num_bytes(number)), '\0');
    BN_bn2bin(number, reinterpret_cast<unsigned char *>(bytes.data()));
    return base64UrlEncode(std::as_bytes(std::span{bytes.data(), bytes.size()}));
}

TEST(JsonWebTokenTest, VerifiesTheRfc7515Es256Example)
{
    const JsonWebToken holder{.claims = R"({"cpk":{"kty":"EC","crv":"P-256",)"
                                        R"("x":"f83OJ3D2xF1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEU",)"
                                        R"("y":"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0"}})"};
    const auto key = holder.publicKey();
    ASSERT_TRUE(key);

    const auto token = JsonWebToken::decode("eyJhbGciOiJFUzI1NiJ9"
                                            ".eyJpc3MiOiJqb2UiLA0KICJleHAiOjEzMDA4MTkzODAsDQogImh0dHA6Ly9leGFt"
                                            "cGxlLmNvbS9pc19yb290Ijp0cnVlfQ"
                                            ".DtEhU3ljbEg8L38VWAfUAqOyKAM6-Xx-F4GawxaepmXFCgfTjDxw5djxLa8ISlSA"
                                            "pmWQxfKTUJqPP3-Kg6NU1Q");
    ASSERT_TRUE(token);
    EXPECT_EQ(token->claims, "{\"iss\":\"joe\",\r\n \"exp\":1300819380,\r\n \"http://example.com/is_root\":true}");
    EXPECT_TRUE(token->verify(key.get()));

    const std::string detached = "eyJhbGciOiJFUzI1NiJ9..DtEhU3ljbEg8L38VWAfUAqOyKAM6-Xx-F4GawxaepmXFCgfTjDxw5djxLa8"
                                 "ISlSApmWQxfKTUJqPP3-Kg6NU1Q";
    EXPECT_TRUE(verifyDetachedJws(detached, token->claims, key.get()));
    EXPECT_FALSE(verifyDetachedJws(detached, token->claims + " ", key.get()));
    EXPECT_FALSE(verifyDetachedJws(token->signing_input + ".x", token->claims, key.get()));
    EXPECT_FALSE(verifyDetachedJws(detached, token->claims, generateKey("P-384").get()));
}

TEST(JsonWebTokenTest, RejectsMalformedTokens)
{
    EXPECT_FALSE(JsonWebToken::decode(""));
    EXPECT_FALSE(JsonWebToken::decode("a.b"));
    EXPECT_FALSE(JsonWebToken::decode("a.b.c.d"));
    EXPECT_FALSE(JsonWebToken::decode("e30.e30.AA"));
    EXPECT_FALSE(JsonWebToken::decode("eyJhbGciOiJFUzI1NiJ9.e3+.AA"));
    const JsonWebToken no_cpk{.claims = "{}"};
    EXPECT_FALSE(no_cpk.publicKey());
    const JsonWebToken bad_jwk{.claims = R"({"cpk":{"kty":"EC","crv":"P-256","x":"AA","y":"AA"}})"};
    EXPECT_FALSE(bad_jwk.publicKey());
}

TEST(JsonWebTokenTest, VerifiesAnRs256Signature)
{
    const PublicKey key(EVP_RSA_gen(2048), EVP_PKEY_free);
    ASSERT_TRUE(key);
    const std::string header = "eyJhbGciOiJSUzI1NiJ9";
    const std::string payload = R"({"fingerprint":[]})";
    const std::string signing_input = header + ".eyJmaW5nZXJwcmludCI6W119";
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    std::size_t length = 0;
    ASSERT_EQ(EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()), 1);
    ASSERT_EQ(EVP_DigestSign(context.get(), nullptr, &length,
                             reinterpret_cast<const unsigned char *>(signing_input.data()), signing_input.size()),
              1);
    std::string signature(length, '\0');
    ASSERT_EQ(EVP_DigestSign(context.get(), reinterpret_cast<unsigned char *>(signature.data()), &length,
                             reinterpret_cast<const unsigned char *>(signing_input.data()), signing_input.size()),
              1);
    std::string encoded(((length + 2) / 3) * 4, '\0');
    encoded.resize(static_cast<std::size_t>(EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                                                            reinterpret_cast<const unsigned char *>(signature.data()),
                                                            static_cast<int>(length))));
    std::ranges::replace(encoded, '+', '-');
    std::ranges::replace(encoded, '/', '_');
    std::erase(encoded, '=');

    EXPECT_TRUE(verifyDetachedJws(header + ".." + encoded, payload, key.get()));
    EXPECT_FALSE(verifyDetachedJws(header + ".." + encoded, payload + " ", key.get()));
    EXPECT_FALSE(verifyDetachedJws("eyJhbGciOiJSUzM4NCJ9.." + encoded, payload, key.get()));
    EXPECT_FALSE(verifyDetachedJws("eyJhbGciOiJFUzI1NiJ9.." + encoded, payload, key.get()));
}

TEST(JsonWebTokenTest, ReadsTheKeyId)
{
    const auto token = JsonWebToken::decode("eyJhbGciOiJSUzI1NiIsImtpZCI6IjNENUMifQ.e30.AA");
    ASSERT_TRUE(token);
    EXPECT_EQ(token->algorithm, "RS256");
    EXPECT_EQ(token->key_id, "3D5C");
    EXPECT_EQ(token->claims, "{}");
}

TEST(JsonWebKeyTest, DecodesAnRsaKey)
{
    const PrivateKey key(EVP_RSA_gen(2048), EVP_PKEY_free);
    BIGNUM *n = nullptr;
    BIGNUM *e = nullptr;
    ASSERT_EQ(EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_RSA_N, &n), 1);
    ASSERT_EQ(EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_RSA_E, &e), 1);
    const std::unique_ptr<BIGNUM, decltype(&BN_free)> owned_n(n, BN_free);
    const std::unique_ptr<BIGNUM, decltype(&BN_free)> owned_e(e, BN_free);

    const auto decoded = decodeJsonWebKey(R"({"kty":"RSA","use":"sig","kid":"k","n":")" + base64UrlOf(n) +
                                          R"(","e":")" + base64UrlOf(e) + R"("})");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(EVP_PKEY_eq(decoded.get(), key.get()), 1);
    EXPECT_EQ(EVP_PKEY_get_bits(decoded.get()), 2048);
}

TEST(JsonWebKeyTest, DecodesAnEcKey)
{
    const auto key = generateKey("P-256");
    BIGNUM *x = nullptr;
    BIGNUM *y = nullptr;
    ASSERT_EQ(EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_EC_PUB_X, &x), 1);
    ASSERT_EQ(EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_EC_PUB_Y, &y), 1);
    const std::unique_ptr<BIGNUM, decltype(&BN_free)> owned_x(x, BN_free);
    const std::unique_ptr<BIGNUM, decltype(&BN_free)> owned_y(y, BN_free);
    std::string x_bytes(32, '\0');
    std::string y_bytes(32, '\0');
    BN_bn2binpad(x, reinterpret_cast<unsigned char *>(x_bytes.data()), 32);
    BN_bn2binpad(y, reinterpret_cast<unsigned char *>(y_bytes.data()), 32);

    const auto decoded = decodeJsonWebKey(R"({"kty":"EC","crv":"P-256","x":")" +
                                          base64UrlEncode(std::as_bytes(std::span{x_bytes.data(), 32})) + R"(","y":")" +
                                          base64UrlEncode(std::as_bytes(std::span{y_bytes.data(), 32})) + R"("})");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(EVP_PKEY_eq(decoded.get(), key.get()), 1);
}

TEST(JsonWebKeyTest, RejectsWhatItCannotRead)
{
    EXPECT_FALSE(decodeJsonWebKey(""));
    EXPECT_FALSE(decodeJsonWebKey("{}"));
    EXPECT_FALSE(decodeJsonWebKey(R"({"kty":"oct","k":"AA"})"));
    EXPECT_FALSE(decodeJsonWebKey(R"({"kty":"RSA","n":"AQAB"})"));
    EXPECT_FALSE(decodeJsonWebKey(R"({"kty":"RSA","n":"","e":"AQAB"})"));
    EXPECT_FALSE(decodeJsonWebKey(R"({"kty":"EC","crv":"P-192","x":"AA","y":"AA"})"));
    EXPECT_FALSE(decodeJsonWebKey(R"({"kty":"EC","crv":"P-256","x":"AA","y":"AA"})"));
}

}  // namespace
}  // namespace endgate
