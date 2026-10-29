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

struct ClientCpkClaim {
    std::string cpk;

    struct glaze {
        using T = ClientCpkClaim;
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
constexpr glz::opts LENIENT_READ{.error_on_unknown_keys = false, .error_on_missing_keys = true};

std::span<const std::byte> bytesOf(std::string_view text)
{
    return std::as_bytes(std::span{text.data(), text.size()});
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
    const auto token_signature = signJws(key_.get(), token_head);
    const auto jws_signature = signJws(key_.get(), jws_head + "." + base64UrlEncode(bytesOf(fingerprints)));
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
    constexpr std::string_view fingerprint_prefix = "a=fingerprint:";
    FingerprintList list;
    for (const auto line : linesOf(sdp)) {
        if (!startsWithIgnoringCase(line, fingerprint_prefix)) {
            continue;
        }
        const auto value = line.substr(fingerprint_prefix.size());
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

std::optional<JsonWebToken> ClientIdentityAssertionVerifier::verify(std::string_view sdp)
{
    const auto assertion = IdentityAssertion::extractFromSdp(sdp);
    const auto token = assertion ? assertion->getToken() : std::nullopt;
    if (!token || token->size() > MAX_TOKEN_SIZE) {
        return std::nullopt;
    }
    auto decoded = JsonWebToken::decode(*token);
    ClientCpkClaim claim;
    if (!decoded || glz::read<LENIENT_READ>(claim, decoded->claims)) {
        return std::nullopt;
    }
    const auto key = decodeSubjectPublicKeyInfo(claim.cpk);
    if (!key || !assertion->verifyFingerprints(key.get())) {
        return std::nullopt;
    }
    return decoded;
}

}  // namespace endgate::nethernet
