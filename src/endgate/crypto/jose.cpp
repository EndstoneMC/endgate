#include "endgate/crypto/jose.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <ranges>
#include <utility>
#include <variant>

#include <glaze/json.hpp>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/param_build.h>
#include <openssl/x509.h>

namespace endgate {

namespace {

struct JwsHeader {
    std::string alg;
    std::optional<std::string> kid;
    std::optional<std::string> x5u;

    struct glaze {
        using T = JwsHeader;
        static constexpr auto value = glz::object("alg", &T::alg, "kid", &T::kid, "x5u", &T::x5u);
    };
};

struct JsonWebKey {
    std::string kty;
    std::optional<std::string> crv;
    std::optional<std::string> x;
    std::optional<std::string> y;
    std::optional<std::string> n;
    std::optional<std::string> e;

    struct glaze {
        using T = JsonWebKey;
        static constexpr auto value =
            glz::object("kty", &T::kty, "crv", &T::crv, "x", &T::x, "y", &T::y, "n", &T::n, "e", &T::e);
    };
};

struct CpkClaim {
    std::variant<JsonWebKey, std::string> cpk;

    struct glaze {
        using T = CpkClaim;
        static constexpr auto value = glz::object("cpk", &T::cpk);
    };
};

constexpr glz::opts LENIENT_READ{.error_on_unknown_keys = false, .error_on_missing_keys = true};

constexpr std::array<EllipticCurve, 3> CURVES{{
    {.algorithm = "ES256",
     .jwk_name = "P-256",
     .group_name = "prime256v1",
     .bits = 256,
     .coordinate_size = 32,
     .digest = EVP_sha256},
    {.algorithm = "ES384",
     .jwk_name = "P-384",
     .group_name = "secp384r1",
     .bits = 384,
     .coordinate_size = 48,
     .digest = EVP_sha384},
    {.algorithm = "ES512",
     .jwk_name = "P-521",
     .group_name = "secp521r1",
     .bits = 521,
     .coordinate_size = 66,
     .digest = EVP_sha512},
}};

std::span<const std::byte> bytesOf(std::string_view text)
{
    return std::as_bytes(std::span{text.data(), text.size()});
}

std::string textOf(const std::vector<std::byte> &bytes)
{
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

bool verify(EVP_PKEY *key, std::string_view algorithm, std::string_view input, std::span<const std::byte> raw)
{
    if (key != nullptr && EVP_PKEY_get_base_id(key) == EVP_PKEY_RSA) {
        constexpr std::array<std::pair<std::string_view, const EVP_MD *(*)()>, 3> rsa_algorithms{{
            {"RS256", EVP_sha256},
            {"RS384", EVP_sha384},
            {"RS512", EVP_sha512},
        }};
        const auto *const end = rsa_algorithms.data() + rsa_algorithms.size();
        const auto *const rsa =
            std::ranges::find(rsa_algorithms.data(), end, algorithm, &decltype(rsa_algorithms)::value_type::first);
        const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        return rsa != end && context &&
               EVP_DigestVerifyInit(context.get(), nullptr, rsa->second(), nullptr, key) == 1 &&
               EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char *>(raw.data()), raw.size(),
                                reinterpret_cast<const unsigned char *>(input.data()), input.size()) == 1;
    }
    const auto *const curve = curveOf(key);
    if (curve == nullptr || curve->algorithm != algorithm || raw.size() != curve->coordinate_size * 2) {
        return false;
    }
    const auto size = static_cast<int>(curve->coordinate_size);
    std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> signature(ECDSA_SIG_new(), ECDSA_SIG_free);
    BIGNUM *r = BN_bin2bn(reinterpret_cast<const unsigned char *>(raw.data()), size, nullptr);
    BIGNUM *s = BN_bin2bn(reinterpret_cast<const unsigned char *>(raw.data()) + size, size, nullptr);
    if (!signature || r == nullptr || s == nullptr || ECDSA_SIG_set0(signature.get(), r, s) != 1) {
        BN_free(r);
        BN_free(s);
        return false;
    }
    unsigned char *der = nullptr;
    const auto der_length = i2d_ECDSA_SIG(signature.get(), &der);
    const std::unique_ptr<unsigned char, decltype([](unsigned char *p) { OPENSSL_free(p); })> owned(der);
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return der_length > 0 && context &&
           EVP_DigestVerifyInit(context.get(), nullptr, curve->digest(), nullptr, key) == 1 &&
           EVP_DigestVerify(context.get(), der, static_cast<std::size_t>(der_length),
                            reinterpret_cast<const unsigned char *>(input.data()), input.size()) == 1;
}

PublicKey toPublicKey(const JsonWebKey &jwk)
{
    const std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder(OSSL_PARAM_BLD_new(),
                                                                                  OSSL_PARAM_BLD_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> modulus(nullptr, BN_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> exponent(nullptr, BN_free);
    std::vector<unsigned char> point{0x04};
    std::string group;
    if (!builder) {
        return nullptr;
    }
    if (jwk.kty == "RSA") {
        const auto n = base64UrlDecode(jwk.n.value_or(""));
        const auto e = base64UrlDecode(jwk.e.value_or(""));
        if (!n || !e || n->empty() || e->empty()) {
            return nullptr;
        }
        modulus.reset(
            BN_bin2bn(reinterpret_cast<const unsigned char *>(n->data()), static_cast<int>(n->size()), nullptr));
        exponent.reset(
            BN_bin2bn(reinterpret_cast<const unsigned char *>(e->data()), static_cast<int>(e->size()), nullptr));
        if (!modulus || !exponent || OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N, modulus.get()) != 1 ||
            OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, exponent.get()) != 1) {
            return nullptr;
        }
    }
    else if (jwk.kty == "EC") {
        const auto *const end = CURVES.data() + CURVES.size();
        const auto *const curve = std::ranges::find(CURVES.data(), end, jwk.crv.value_or(""), &EllipticCurve::jwk_name);
        const auto x = base64UrlDecode(jwk.x.value_or(""));
        const auto y = base64UrlDecode(jwk.y.value_or(""));
        if (curve == end || !x || !y || x->size() != curve->coordinate_size || y->size() != curve->coordinate_size) {
            return nullptr;
        }
        for (const auto byte : *x) {
            point.push_back(std::to_integer<unsigned char>(byte));
        }
        for (const auto byte : *y) {
            point.push_back(std::to_integer<unsigned char>(byte));
        }
        group = curve->group_name;
        if (OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, group.c_str(), 0) != 1 ||
            OSSL_PARAM_BLD_push_octet_string(builder.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) != 1) {
            return nullptr;
        }
    }
    else {
        return nullptr;
    }
    const std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> params(OSSL_PARAM_BLD_to_param(builder.get()),
                                                                         OSSL_PARAM_free);
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_from_name(nullptr, jwk.kty.c_str(), nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *key = nullptr;
    if (!params || !context || EVP_PKEY_fromdata_init(context.get()) != 1 ||
        EVP_PKEY_fromdata(context.get(), &key, EVP_PKEY_PUBLIC_KEY, params.get()) != 1) {
        return nullptr;
    }
    return {key, EVP_PKEY_free};
}

}  // namespace

std::string base64Encode(std::span<const std::byte> bytes)
{
    std::string encoded(((bytes.size() + 2) / 3) * 4, '\0');
    const auto length =
        EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                        reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()));
    encoded.resize(static_cast<std::size_t>(std::max(length, 0)));
    return encoded;
}

std::optional<std::vector<std::byte>> base64Decode(std::string_view text)
{
    const auto padding =
        static_cast<std::size_t>(std::ranges::count(text | std::views::reverse | std::views::take(2), '='));
    const auto valid = std::ranges::all_of(text.substr(0, text.size() - padding), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/';
    });
    if (text.size() % 4 != 0 || !valid || text.size() > 16U << 20U) {
        return std::nullopt;
    }
    std::vector<std::byte> decoded(text.size() / 4 * 3);
    const auto length =
        EVP_DecodeBlock(reinterpret_cast<unsigned char *>(decoded.data()),
                        reinterpret_cast<const unsigned char *>(text.data()), static_cast<int>(text.size()));
    if (length < 0 || std::cmp_less(length, padding)) {
        return std::nullopt;
    }
    decoded.resize(static_cast<std::size_t>(length) - padding);
    return decoded;
}

std::string base64UrlEncode(std::span<const std::byte> bytes)
{
    auto encoded = base64Encode(bytes);
    std::ranges::replace(encoded, '+', '-');
    std::ranges::replace(encoded, '/', '_');
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
}

std::optional<std::vector<std::byte>> base64UrlDecode(std::string_view text)
{
    if (std::ranges::any_of(text, [](char c) { return c == '+' || c == '/' || c == '='; })) {
        return std::nullopt;
    }
    std::string standard(text);
    std::ranges::replace(standard, '-', '+');
    std::ranges::replace(standard, '_', '/');
    standard.append((4 - standard.size() % 4) % 4, '=');
    return base64Decode(standard);
}

const EllipticCurve *curveOf(EVP_PKEY *key)
{
    if (key == nullptr || EVP_PKEY_get_base_id(key) != EVP_PKEY_EC) {
        return nullptr;
    }
    const auto bits = EVP_PKEY_get_bits(key);
    const auto *const end = CURVES.data() + CURVES.size();
    const auto *const curve = std::ranges::find(CURVES.data(), end, bits, &EllipticCurve::bits);
    return curve == end ? nullptr : curve;
}

std::optional<std::string> signJws(EVP_PKEY *key, std::string_view input)
{
    const auto *const curve = curveOf(key);
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    std::size_t length = 0;
    if (curve == nullptr || !context ||
        EVP_DigestSignInit(context.get(), nullptr, curve->digest(), nullptr, key) != 1 ||
        EVP_DigestSign(context.get(), nullptr, &length, reinterpret_cast<const unsigned char *>(input.data()),
                       input.size()) != 1) {
        return std::nullopt;
    }
    std::vector<unsigned char> der(length);
    if (EVP_DigestSign(context.get(), der.data(), &length, reinterpret_cast<const unsigned char *>(input.data()),
                       input.size()) != 1) {
        return std::nullopt;
    }
    const unsigned char *cursor = der.data();
    const std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> signature(
        d2i_ECDSA_SIG(nullptr, &cursor, static_cast<int>(length)), ECDSA_SIG_free);
    if (!signature) {
        return std::nullopt;
    }
    std::vector<std::byte> raw(curve->coordinate_size * 2);
    const auto size = static_cast<int>(curve->coordinate_size);
    if (BN_bn2binpad(ECDSA_SIG_get0_r(signature.get()), reinterpret_cast<unsigned char *>(raw.data()), size) != size ||
        BN_bn2binpad(ECDSA_SIG_get0_s(signature.get()), reinterpret_cast<unsigned char *>(raw.data()) + size, size) !=
            size) {
        return std::nullopt;
    }
    return base64UrlEncode(raw);
}

PublicKey decodeJsonWebKey(std::string_view json)
{
    JsonWebKey jwk;
    if (glz::read<LENIENT_READ>(jwk, json)) {
        return nullptr;
    }
    return toPublicKey(jwk);
}

PublicKey decodeSubjectPublicKeyInfo(std::string_view base64)
{
    const auto der = base64Decode(base64);
    if (!der) {
        return nullptr;
    }
    const auto *cursor = reinterpret_cast<const unsigned char *>(der->data());
    return {d2i_PUBKEY(nullptr, &cursor, static_cast<int>(der->size())), EVP_PKEY_free};
}

std::optional<JsonWebToken> JsonWebToken::decode(std::string_view token, std::size_t max_size)
{
    const auto first = token.find('.');
    const auto second = first == std::string_view::npos ? std::string_view::npos : token.find('.', first + 1);
    if (second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos ||
        token.size() > max_size) {
        return std::nullopt;
    }
    const auto header = base64UrlDecode(token.substr(0, first));
    const auto claims = base64UrlDecode(token.substr(first + 1, second - first - 1));
    const auto signature = base64UrlDecode(token.substr(second + 1));
    JwsHeader parsed;
    if (!header || !claims || !signature || glz::read<LENIENT_READ>(parsed, textOf(*header))) {
        return std::nullopt;
    }
    return JsonWebToken{.header = textOf(*header),
                        .claims = textOf(*claims),
                        .algorithm = parsed.alg,
                        .key_id = parsed.kid.value_or(""),
                        .x5u = parsed.x5u.value_or(""),
                        .signing_input = std::string(token.substr(0, second)),
                        .signature = *signature};
}

bool JsonWebToken::verify(EVP_PKEY *key) const
{
    return endgate::verify(key, algorithm, signing_input, signature);
}

PublicKey JsonWebToken::publicKey() const
{
    CpkClaim claim;
    if (glz::read<LENIENT_READ>(claim, claims)) {
        return nullptr;
    }
    if (const auto *const spki = std::get_if<std::string>(&claim.cpk)) {
        return decodeSubjectPublicKeyInfo(*spki);
    }
    return toPublicKey(std::get<JsonWebKey>(claim.cpk));
}

bool verifyDetachedJws(std::string_view jws, std::string_view payload, EVP_PKEY *key)
{
    const auto first = jws.find('.');
    if (first == std::string_view::npos || jws.substr(first).size() < 2 || jws[first + 1] != '.' ||
        jws.find('.', first + 2) != std::string_view::npos) {
        return false;
    }
    const auto header = base64UrlDecode(jws.substr(0, first));
    const auto signature = base64UrlDecode(jws.substr(first + 2));
    JwsHeader parsed;
    if (!header || !signature || glz::read<LENIENT_READ>(parsed, textOf(*header))) {
        return false;
    }
    const auto signing_input = std::string(jws.substr(0, first)) + "." + base64UrlEncode(bytesOf(payload));
    return verify(key, parsed.alg, signing_input, *signature);
}

}  // namespace endgate
