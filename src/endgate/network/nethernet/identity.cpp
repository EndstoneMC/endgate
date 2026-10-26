#include "endgate/network/nethernet/identity.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <ranges>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>

#include <glaze/json.hpp>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/encoder.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace endgate::nethernet {

namespace {

struct FingerprintEntry {
    std::string algorithm;
    std::string digest;

    struct glaze {
        using T = FingerprintEntry;
        static constexpr auto value = glz::object("algorithm", &T::algorithm, "digest", &T::digest);
    };
};

struct FingerprintList {
    std::vector<FingerprintEntry> fingerprint;

    struct glaze {
        using T = FingerprintList;
        static constexpr auto value = glz::object("fingerprint", &T::fingerprint);
    };
};

struct Idp {
    std::string domain;
    std::string protocol;

    struct glaze {
        using T = Idp;
        static constexpr auto value = glz::object("domain", &T::domain, "protocol", &T::protocol);
    };
};

struct Envelope {
    std::string assertion;
    Idp idp;

    struct glaze {
        using T = Envelope;
        static constexpr auto value = glz::object("assertion", &T::assertion, "idp", &T::idp);
    };
};

struct Assertion {
    std::string fingerprints;
    std::string token;

    struct glaze {
        using T = Assertion;
        static constexpr auto value = glz::object("fingerprints", &T::fingerprints, "token", &T::token);
    };
};

struct JwsHeader {
    std::string alg;

    struct glaze {
        using T = JwsHeader;
        static constexpr auto value = glz::object("alg", &T::alg);
    };
};

struct JwtHeader {
    std::string alg;
    std::string x5u;

    struct glaze {
        using T = JwtHeader;
        static constexpr auto value = glz::object("alg", &T::alg, "x5u", &T::x5u);
    };
};

struct JsonWebKey {
    std::string crv;
    std::string kty;
    std::string x;
    std::string y;

    struct glaze {
        using T = JsonWebKey;
        static constexpr auto value = glz::object("crv", &T::crv, "kty", &T::kty, "x", &T::x, "y", &T::y);
    };
};

struct CpkClaim {
    std::variant<JsonWebKey, std::string> cpk;

    struct glaze {
        using T = CpkClaim;
        static constexpr auto value = glz::object("cpk", &T::cpk);
    };
};

template <typename Cpk>
struct Claims {
    Cpk cpk;
    std::int64_t exp;
    std::int64_t iat;

    struct glaze {
        using T = Claims;
        static constexpr auto value = glz::object("cpk", &T::cpk, "exp", &T::exp, "iat", &T::iat);
    };
};

constexpr std::string_view IDENTITY_PREFIX = "a=identity:";
constexpr std::string_view FINGERPRINT_PREFIX = "a=fingerprint:";
constexpr glz::opts LENIENT_READ{.error_on_unknown_keys = false, .error_on_missing_keys = true};

struct Curve {
    std::string_view algorithm;
    std::string_view jwk_name;
    std::string_view group_name;
    int bits;
    std::size_t coordinate_size;
    const EVP_MD *(*digest)();
};

constexpr std::array<Curve, 3> CURVES{{
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

const Curve *curveOf(EVP_PKEY *key)
{
    if (key == nullptr || EVP_PKEY_get_base_id(key) != EVP_PKEY_EC) {
        return nullptr;
    }
    const auto bits = EVP_PKEY_get_bits(key);
    const auto *const curve = std::ranges::find(CURVES, bits, &Curve::bits);
    return curve == CURVES.end() ? nullptr : curve;
}

std::span<const std::byte> bytesOf(std::string_view text)
{
    return std::as_bytes(std::span{text.data(), text.size()});
}

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
    if (text.size() % 4 != 0 || !valid || text.size() > 1U << 20U) {
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

std::string textOf(const std::vector<std::byte> &bytes)
{
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

std::vector<std::string_view> linesOf(std::string_view text)
{
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const auto end = text.find_first_of("\r\n");
        lines.push_back(text.substr(0, end));
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + (text.substr(end).starts_with("\r\n") ? 2 : 1));
    }
    return lines;
}

bool startsWithIgnoringCase(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() &&
           std::ranges::equal(text.substr(0, prefix.size()), prefix, [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

std::optional<std::string> sign(EVP_PKEY *key, std::string_view input)
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
        d2i_ECDSA_SIG(nullptr, &cursor, static_cast<std::int64_t>(length)), ECDSA_SIG_free);
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

bool verify(EVP_PKEY *key, std::string_view algorithm, std::string_view input, std::span<const std::byte> raw)
{
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

template <typename Value>
std::string writeJson(const Value &value)
{
    std::string json;
    if (glz::write_json(value, json)) {
        return {};
    }
    return json;
}

}  // namespace

std::optional<JsonWebToken> JsonWebToken::decode(std::string_view token)
{
    const auto first = token.find('.');
    const auto second = first == std::string_view::npos ? std::string_view::npos : token.find('.', first + 1);
    if (second == std::string_view::npos || token.find('.', second + 1) != std::string_view::npos ||
        token.size() > 1U << 16U) {
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
                        .signing_input = std::string(token.substr(0, second)),
                        .signature = *signature};
}

bool JsonWebToken::verify(EVP_PKEY *key) const
{
    return nethernet::verify(key, algorithm, signing_input, signature);
}

PublicKey JsonWebToken::publicKey() const
{
    CpkClaim claim;
    if (glz::read<LENIENT_READ>(claim, claims)) {
        return nullptr;
    }
    if (const auto *const spki = std::get_if<std::string>(&claim.cpk)) {
        const auto der = base64Decode(*spki);
        if (!der) {
            return nullptr;
        }
        const auto *cursor = reinterpret_cast<const unsigned char *>(der->data());
        return {d2i_PUBKEY(nullptr, &cursor, static_cast<std::int64_t>(der->size())), EVP_PKEY_free};
    }
    const auto &jwk = std::get<JsonWebKey>(claim.cpk);
    const auto *const curve = std::ranges::find(CURVES, jwk.crv, &Curve::jwk_name);
    const auto x = base64UrlDecode(jwk.x);
    const auto y = base64UrlDecode(jwk.y);
    if (jwk.kty != "EC" || curve == CURVES.end() || !x || !y || x->size() != curve->coordinate_size ||
        y->size() != curve->coordinate_size) {
        return nullptr;
    }
    std::vector<unsigned char> point{0x04};
    for (const auto byte : *x) {
        point.push_back(std::to_integer<unsigned char>(byte));
    }
    for (const auto byte : *y) {
        point.push_back(std::to_integer<unsigned char>(byte));
    }
    const std::string group(curve->group_name);
    const std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder(OSSL_PARAM_BLD_new(),
                                                                                  OSSL_PARAM_BLD_free);
    if (!builder || OSSL_PARAM_BLD_push_utf8_string(builder.get(), OSSL_PKEY_PARAM_GROUP_NAME, group.c_str(), 0) != 1 ||
        OSSL_PARAM_BLD_push_octet_string(builder.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) != 1) {
        return nullptr;
    }
    const std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> params(OSSL_PARAM_BLD_to_param(builder.get()),
                                                                         OSSL_PARAM_free);
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
        EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY *key = nullptr;
    if (!params || !context || EVP_PKEY_fromdata_init(context.get()) != 1 ||
        EVP_PKEY_fromdata(context.get(), &key, EVP_PKEY_PUBLIC_KEY, params.get()) != 1) {
        return nullptr;
    }
    return {key, EVP_PKEY_free};
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

ServerIdentityAssertionGenerator::ServerIdentityAssertionGenerator(PrivateKey key, Config config)
    : key_(std::move(key)), config_(std::move(config))
{
    if (curveOf(key_.get()) == nullptr) {
        throw std::invalid_argument("the identity key must be an ECDSA P-256, P-384 or P-521 key");
    }
}

PrivateKey ServerIdentityAssertionGenerator::loadOrCreateKey(const std::filesystem::path &path)
{
    if (std::filesystem::exists(path)) {
        const std::unique_ptr<BIO, decltype(&BIO_free)> file(BIO_new_file(path.string().c_str(), "r"), BIO_free);
        PrivateKey key(file ? PEM_read_bio_PrivateKey(file.get(), nullptr, nullptr, nullptr) : nullptr, EVP_PKEY_free);
        const auto *const curve = curveOf(key.get());
        if (curve == nullptr || curve->jwk_name != "P-384") {
            throw std::runtime_error("invalid server identity key file " + path.string());
        }
        return key;
    }

    PrivateKey key(EVP_EC_gen("P-384"), EVP_PKEY_free);
    if (!key) {
        throw std::runtime_error("failed to generate a server identity key");
    }
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    auto temporary = path;
    temporary += ".tmp";
    {
        const std::unique_ptr<BIO, decltype(&BIO_free)> file(BIO_new_file(temporary.string().c_str(), "w"), BIO_free);
        if (!file) {
            throw std::runtime_error("unable to write server identity key file " + temporary.string());
        }
        std::filesystem::permissions(temporary,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        const std::unique_ptr<OSSL_ENCODER_CTX, decltype(&OSSL_ENCODER_CTX_free)> encoder(
            OSSL_ENCODER_CTX_new_for_pkey(key.get(), OSSL_KEYMGMT_SELECT_KEYPAIR, "PEM", "type-specific", nullptr),
            OSSL_ENCODER_CTX_free);
        if (!encoder || OSSL_ENCODER_to_bio(encoder.get(), file.get()) != 1) {
            std::filesystem::remove(temporary);
            throw std::runtime_error("unable to write server identity key file " + temporary.string());
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("unable to replace server identity key file " + path.string());
    }
    return key;
}

std::optional<std::string> ServerIdentityAssertionGenerator::createAssertion(
    std::string_view fingerprints, std::chrono::system_clock::time_point now) const
{
    const auto *const curve = curveOf(key_.get());
    const auto issued = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const auto expires = issued + config_.token_lifetime.count();

    unsigned char *der = nullptr;
    const auto der_length = i2d_PUBKEY(key_.get(), &der);
    const std::unique_ptr<unsigned char, decltype([](unsigned char *p) { OPENSSL_free(p); })> owned_der(der);
    if (der_length <= 0) {
        return std::nullopt;
    }
    const auto public_key = base64Encode(std::as_bytes(std::span{der, static_cast<std::size_t>(der_length)}));

    std::string claims;
    if (config_.cpk_encoding == CpkEncoding::JsonWebKey) {
        BIGNUM *x = nullptr;
        BIGNUM *y = nullptr;
        EVP_PKEY_get_bn_param(key_.get(), OSSL_PKEY_PARAM_EC_PUB_X, &x);
        EVP_PKEY_get_bn_param(key_.get(), OSSL_PKEY_PARAM_EC_PUB_Y, &y);
        const std::unique_ptr<BIGNUM, decltype(&BN_free)> owned_x(x, BN_free);
        const std::unique_ptr<BIGNUM, decltype(&BN_free)> owned_y(y, BN_free);
        std::vector<std::byte> x_bytes(curve->coordinate_size);
        std::vector<std::byte> y_bytes(curve->coordinate_size);
        const auto size = static_cast<int>(curve->coordinate_size);
        if (x == nullptr || y == nullptr ||
            BN_bn2binpad(x, reinterpret_cast<unsigned char *>(x_bytes.data()), size) != size ||
            BN_bn2binpad(y, reinterpret_cast<unsigned char *>(y_bytes.data()), size) != size) {
            return std::nullopt;
        }
        claims = writeJson(Claims<JsonWebKey>{.cpk = {.crv = std::string(curve->jwk_name),
                                                      .kty = "EC",
                                                      .x = base64UrlEncode(x_bytes),
                                                      .y = base64UrlEncode(y_bytes)},
                                              .exp = expires,
                                              .iat = issued});
    }
    else {
        claims = writeJson(Claims<std::string>{.cpk = public_key, .exp = expires, .iat = issued});
    }

    const auto token_head =
        base64UrlEncode(bytesOf(writeJson(JwtHeader{.alg = std::string(curve->algorithm), .x5u = public_key}))) + "." +
        base64UrlEncode(bytesOf(claims));
    const auto jws_head = base64UrlEncode(bytesOf(writeJson(JwsHeader{.alg = std::string(curve->algorithm)})));
    const auto token_signature = sign(key_.get(), token_head);
    const auto jws_signature = sign(key_.get(), jws_head + "." + base64UrlEncode(bytesOf(fingerprints)));
    if (claims.empty() || !token_signature || !jws_signature) {
        return std::nullopt;
    }
    return writeJson(
        Assertion{.fingerprints = jws_head + ".." + *jws_signature, .token = token_head + "." + *token_signature});
}

std::optional<IdentityAssertion> IdentityAssertion::create(const ServerIdentityAssertionGenerator &generator,
                                                           std::string_view local_sdp,
                                                           std::chrono::system_clock::time_point now)
{
    auto fingerprints = fingerprintsOf(local_sdp);
    if (fingerprints == R"({"fingerprint":[]})") {
        return std::nullopt;
    }
    auto assertion = generator.createAssertion(fingerprints, now);
    if (!assertion) {
        return std::nullopt;
    }
    IdentityAssertion identity;
    identity.assertion_ = std::move(*assertion);
    identity.idp_domain_ = generator.idpDomain();
    identity.idp_protocol_ = "default";
    identity.fingerprints_ = std::move(fingerprints);
    return identity;
}

std::optional<IdentityAssertion> IdentityAssertion::extractFromSdp(std::string_view sdp)
{
    const auto lines = linesOf(sdp);
    const auto line = std::ranges::find_if(
        lines, [](std::string_view candidate) { return startsWithIgnoringCase(candidate, IDENTITY_PREFIX); });
    if (line == lines.end()) {
        return std::nullopt;
    }
    const auto decoded = base64Decode(line->substr(IDENTITY_PREFIX.size()));
    Envelope envelope;
    if (!decoded || glz::read<LENIENT_READ>(envelope, textOf(*decoded))) {
        return std::nullopt;
    }
    auto fingerprints = fingerprintsOf(sdp);
    if (fingerprints == R"({"fingerprint":[]})") {
        return std::nullopt;
    }
    IdentityAssertion identity;
    identity.assertion_ = std::move(envelope.assertion);
    identity.idp_domain_ = std::move(envelope.idp.domain);
    identity.idp_protocol_ = std::move(envelope.idp.protocol);
    identity.fingerprints_ = std::move(fingerprints);
    return identity;
}

std::string IdentityAssertion::injectIntoSdp(std::string_view sdp) const
{
    const auto envelope =
        writeJson(Envelope{.assertion = assertion_, .idp = {.domain = idp_domain_, .protocol = idp_protocol_}});
    const auto identity_line = std::string(IDENTITY_PREFIX) + base64Encode(bytesOf(envelope));
    auto lines = linesOf(sdp);
    const auto media = std::ranges::find_if(lines, [](std::string_view line) { return line.starts_with("m="); });
    lines.insert(media, identity_line);
    std::string result;
    for (const auto line : lines) {
        result += line;
        result += "\r\n";
    }
    return result;
}

std::string IdentityAssertion::stripFromSdp(std::string_view sdp)
{
    std::string result;
    for (const auto line : linesOf(sdp)) {
        if (!startsWithIgnoringCase(line, IDENTITY_PREFIX)) {
            result += line;
            result += "\r\n";
        }
    }
    return result;
}

std::string IdentityAssertion::fingerprintsOf(std::string_view sdp)
{
    FingerprintList list;
    for (const auto line : linesOf(sdp)) {
        if (!startsWithIgnoringCase(line, FINGERPRINT_PREFIX)) {
            continue;
        }
        const auto value = line.substr(FINGERPRINT_PREFIX.size());
        const auto space = value.find(' ');
        if (space == std::string_view::npos || space == 0 || space + 1 == value.size()) {
            continue;
        }
        list.fingerprint.push_back(
            {.algorithm = std::string(value.substr(0, space)), .digest = std::string(value.substr(space + 1))});
    }
    std::ranges::sort(list.fingerprint, {},
                      [](const FingerprintEntry &entry) { return std::tie(entry.algorithm, entry.digest); });
    return writeJson(list);
}

std::optional<std::string> IdentityAssertion::getToken() const
{
    Assertion parsed;
    if (glz::read<LENIENT_READ>(parsed, assertion_)) {
        return std::nullopt;
    }
    return parsed.token;
}

std::optional<std::string> IdentityAssertion::getSignedFingerprints() const
{
    Assertion parsed;
    if (glz::read<LENIENT_READ>(parsed, assertion_)) {
        return std::nullopt;
    }
    return parsed.fingerprints;
}

bool IdentityAssertion::verifyFingerprints(EVP_PKEY *key) const
{
    const auto jws = getSignedFingerprints();
    return jws && verifyDetachedJws(*jws, fingerprints_, key);
}

}  // namespace endgate::nethernet
