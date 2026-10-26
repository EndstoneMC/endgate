#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

namespace endgate::nethernet {

using PublicKey = std::shared_ptr<EVP_PKEY>;
using PrivateKey = std::shared_ptr<EVP_PKEY>;

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
    std::string signing_input;
    std::vector<std::byte> signature;

    [[nodiscard]] static std::optional<JsonWebToken> decode(std::string_view token);

    /**
     * @see RFC 7518 section 3.4
     */
    [[nodiscard]] bool verify(EVP_PKEY *key) const;

    /**
     * Reads the cpk claim, accepting both a JSON Web Key and a base64 DER SubjectPublicKeyInfo.
     */
    [[nodiscard]] PublicKey publicKey() const;
};

/**
 * @see RFC 7515 appendix F
 */
[[nodiscard]] bool verifyDetachedJws(std::string_view jws, std::string_view payload, EVP_PKEY *key);

enum class CpkEncoding {
    JsonWebKey,
    SubjectPublicKeyInfo,
};

/**
 * Signs the server's identity into an answer: a self-signed JWT carrying the operator key as cpk,
 * and a detached JWS over the answer's fingerprints, both under that one long-lived key.
 *
 * @see ServerIdentityAssertionGenerator
 */
class ServerIdentityAssertionGenerator {
public:
    struct Config {
        std::string domain;
        std::chrono::seconds token_lifetime{std::chrono::days{30}};
        CpkEncoding cpk_encoding{CpkEncoding::JsonWebKey};
    };

    ServerIdentityAssertionGenerator(PrivateKey key, Config config);

    /**
     * Reads the operator key from a PEM file, generating and saving a P-384 key there first if there
     * is none.
     */
    [[nodiscard]] static PrivateKey loadOrCreateKey(const std::filesystem::path &path);

    /**
     * @see ServerIdentityAssertionGenerator::createAssertion
     */
    [[nodiscard]] std::optional<std::string> createAssertion(std::string_view fingerprints,
                                                             std::chrono::system_clock::time_point now) const;

    [[nodiscard]] std::string_view idpDomain() const { return config_.domain; }
    [[nodiscard]] EVP_PKEY *getKey() const { return key_.get(); }

private:
    PrivateKey key_;
    Config config_;
};

/**
 * The a=identity envelope of an SDP: an identity provider and its opaque assertion, bound to the
 * SDP's fingerprints.
 *
 * @see NetherNet::IdentityAssertion
 */
class IdentityAssertion {
public:
    /**
     * @see NetherNet::IdentityAssertion::create
     */
    [[nodiscard]] static std::optional<IdentityAssertion> create(const ServerIdentityAssertionGenerator &generator,
                                                                 std::string_view local_sdp,
                                                                 std::chrono::system_clock::time_point now);

    /**
     * @see NetherNet::IdentityAssertion::extractFromSdp
     */
    [[nodiscard]] static std::optional<IdentityAssertion> extractFromSdp(std::string_view sdp);

    /**
     * @see NetherNet::IdentityAssertion::injectIntoSdp
     */
    [[nodiscard]] std::string injectIntoSdp(std::string_view sdp) const;

    [[nodiscard]] static std::string stripFromSdp(std::string_view sdp);

    /**
     * The canonical JSON over an SDP's a=fingerprint lines that the assertion signs.
     */
    [[nodiscard]] static std::string fingerprintsOf(std::string_view sdp);

    [[nodiscard]] const std::string &getAssertion() const { return assertion_; }
    [[nodiscard]] const std::string &getIdpDomain() const { return idp_domain_; }
    [[nodiscard]] const std::string &getIdpProtocol() const { return idp_protocol_; }
    [[nodiscard]] const std::string &getFingerprints() const { return fingerprints_; }

    /**
     * The JWT and detached fingerprints JWS the assertion carries, as Mojang's onboarding guide
     * section 5 defines them.
     */
    [[nodiscard]] std::optional<std::string> getToken() const;
    [[nodiscard]] std::optional<std::string> getSignedFingerprints() const;

    /**
     * Checks the assertion's fingerprints JWS against the SDP's fingerprints under the given key.
     */
    [[nodiscard]] bool verifyFingerprints(EVP_PKEY *key) const;

private:
    std::string assertion_;
    std::string idp_domain_;
    std::string idp_protocol_;
    std::string fingerprints_;
};

}  // namespace endgate::nethernet
