#include "endgate/network/rtc/certificate.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <openssl/bn.h>
#include <openssl/ec.h>

namespace endgate::rtc {

namespace {

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs)
{
    return std::ranges::equal(lhs, rhs, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

}  // namespace

std::optional<CertificateFingerprint> CertificateFingerprint::of(X509 *certificate, std::string_view algorithm)
{
    const std::array<std::pair<std::string_view, const EVP_MD *>, 5> digests{{{"sha-1", EVP_sha1()},
                                                                              {"sha-224", EVP_sha224()},
                                                                              {"sha-256", EVP_sha256()},
                                                                              {"sha-384", EVP_sha384()},
                                                                              {"sha-512", EVP_sha512()}}};
    const auto *const end = digests.data() + digests.size();
    const auto *const digest = std::ranges::find_if(
        digests.data(), end, [&](const auto &entry) { return equalsIgnoreCase(entry.first, algorithm); });
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int length = 0;
    if (certificate == nullptr || digest == end ||
        X509_digest(certificate, digest->second, bytes.data(), &length) != 1) {
        return std::nullopt;
    }
    CertificateFingerprint fingerprint{.algorithm = std::string(algorithm), .value = {}};
    std::ranges::transform(fingerprint.algorithm, fingerprint.algorithm.begin(),
                           [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    for (unsigned int i = 0; i < length; ++i) {
        std::array<char, 4> hex{};
        std::snprintf(hex.data(), hex.size(), i == 0 ? "%02X" : ":%02X", bytes[i]);
        fingerprint.value += hex.data();
    }
    return fingerprint;
}

bool CertificateFingerprint::matches(X509 *certificate, const std::vector<CertificateFingerprint> &fingerprints)
{
    for (const auto *preferred : {"sha-512", "sha-384", "sha-256", "sha-224", "sha-1"}) {
        auto offered = fingerprints | std::views::filter([&](const CertificateFingerprint &fingerprint) {
                           return equalsIgnoreCase(fingerprint.algorithm, preferred);
                       });
        if (offered.empty()) {
            continue;
        }
        const auto actual = of(certificate, preferred);
        return actual && std::ranges::any_of(offered, [&](const CertificateFingerprint &fingerprint) {
                   return equalsIgnoreCase(fingerprint.value, actual->value);
               });
    }
    return false;
}

Certificate::Certificate(std::unique_ptr<X509, decltype(&X509_free)> x509,
                         std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key, CertificateFingerprint fingerprint)
    : x509_(std::move(x509)), key_(std::move(key)), fingerprint_(std::move(fingerprint))
{
}

std::shared_ptr<const Certificate> Certificate::generate()
{
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_EC_gen("P-256"), EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> x509(X509_new(), X509_free);
    const std::unique_ptr<BIGNUM, decltype(&BN_free)> serial(BN_new(), BN_free);
    const std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> name(X509_NAME_new(), X509_NAME_free);
    constexpr std::string_view common_name = "endgate";
    constexpr std::int64_t one_day = 60 * 60 * 24;
    if (!key || !x509 || !serial || !name || X509_set_version(x509.get(), X509_VERSION_3) != 1 ||
        BN_rand(serial.get(), 64, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1 ||
        BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(x509.get())) == nullptr ||
        X509_gmtime_adj(X509_getm_notBefore(x509.get()), -one_day) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(x509.get()), 365 * one_day) == nullptr ||
        X509_NAME_add_entry_by_NID(name.get(), NID_commonName, MBSTRING_UTF8,
                                   reinterpret_cast<const unsigned char *>(common_name.data()),
                                   static_cast<int>(common_name.size()), -1, 0) != 1 ||
        X509_set_subject_name(x509.get(), name.get()) != 1 || X509_set_issuer_name(x509.get(), name.get()) != 1 ||
        X509_set_pubkey(x509.get(), key.get()) != 1 || X509_sign(x509.get(), key.get(), EVP_sha256()) == 0) {
        throw std::runtime_error("failed to generate the DTLS certificate");
    }
    auto fingerprint = CertificateFingerprint::of(x509.get(), "sha-256");
    if (!fingerprint) {
        throw std::runtime_error("failed to fingerprint the DTLS certificate");
    }
    return std::shared_ptr<const Certificate>(
        new Certificate(std::move(x509), std::move(key), std::move(*fingerprint)));
}

const std::shared_ptr<const Certificate> &Certificate::shared()
{
    static const auto certificate = generate();
    return certificate;
}

}  // namespace endgate::rtc
