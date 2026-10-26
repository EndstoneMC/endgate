#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/x509.h>

namespace endgate::rtc {

/**
 * @see RFC 8122 section 5
 */
struct CertificateFingerprint {
    std::string algorithm;
    std::string value;

    [[nodiscard]] static std::optional<CertificateFingerprint> of(X509 *certificate, std::string_view algorithm);

    /**
     * @see RFC 8122 section 5.1
     */
    [[nodiscard]] static bool matches(X509 *certificate, const std::vector<CertificateFingerprint> &fingerprints);
};

/**
 * A self-signed ECDSA P-256 certificate and its key, the DTLS identity a PeerConnection presents.
 *
 * @see rtc::impl::Certificate
 * @see RFC 8827 section 6.5
 */
class Certificate {
public:
    [[nodiscard]] static std::shared_ptr<const Certificate> generate();

    /**
     * The certificate shared by every PeerConnection in this process, generated on first use.
     */
    [[nodiscard]] static const std::shared_ptr<const Certificate> &shared();

    [[nodiscard]] X509 *getX509() const { return x509_.get(); }
    [[nodiscard]] EVP_PKEY *getPrivateKey() const { return key_.get(); }
    [[nodiscard]] const CertificateFingerprint &getFingerprint() const { return fingerprint_; }

private:
    Certificate(std::unique_ptr<X509, decltype(&X509_free)> x509,
                std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key, CertificateFingerprint fingerprint);

    std::unique_ptr<X509, decltype(&X509_free)> x509_;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key_;
    CertificateFingerprint fingerprint_;
};

}  // namespace endgate::rtc
