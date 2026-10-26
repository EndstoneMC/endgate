#include <cctype>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/network/rtc/certificate.h"

namespace endgate::rtc {
namespace {

TEST(CertificateTest, FingerprintsWithUppercaseColonSeparatedSha256)
{
    const auto certificate = Certificate::generate();
    const auto &fingerprint = certificate->getFingerprint();
    EXPECT_EQ(fingerprint.algorithm, "sha-256");
    ASSERT_EQ(fingerprint.value.size(), 32U * 3 - 1);
    for (std::size_t i = 0; i < fingerprint.value.size(); ++i) {
        const char c = fingerprint.value[i];
        if (i % 3 == 2) {
            EXPECT_EQ(c, ':');
        }
        else {
            EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(c)) || (c >= 'A' && c <= 'F'));
        }
    }
}

TEST(CertificateTest, UsesAnEcdsaP256Key)
{
    const auto certificate = Certificate::generate();
    EXPECT_EQ(EVP_PKEY_get_base_id(certificate->getPrivateKey()), EVP_PKEY_EC);
    EXPECT_EQ(EVP_PKEY_get_bits(certificate->getPrivateKey()), 256);
    EXPECT_EQ(X509_verify(certificate->getX509(), certificate->getPrivateKey()), 1);
}

TEST(CertificateTest, GeneratesADistinctIdentityEachTime)
{
    EXPECT_NE(Certificate::generate()->getFingerprint().value, Certificate::generate()->getFingerprint().value);
    EXPECT_EQ(Certificate::shared(), Certificate::shared());
}

TEST(CertificateFingerprintTest, MatchesItsOwnCertificateInAnyCase)
{
    const auto certificate = Certificate::generate();
    auto lowercase = certificate->getFingerprint();
    for (auto &c : lowercase.value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    lowercase.algorithm = "SHA-256";
    EXPECT_TRUE(CertificateFingerprint::matches(certificate->getX509(), {certificate->getFingerprint()}));
    EXPECT_TRUE(CertificateFingerprint::matches(certificate->getX509(), {lowercase}));
    EXPECT_FALSE(CertificateFingerprint::matches(Certificate::generate()->getX509(), {certificate->getFingerprint()}));
}

TEST(CertificateFingerprintTest, VerifiesAgainstTheStrongestAlgorithmOffered)
{
    const auto certificate = Certificate::generate();
    const auto other = Certificate::generate();
    const auto sha1 = *CertificateFingerprint::of(certificate->getX509(), "sha-1");
    const auto wrong_sha512 = *CertificateFingerprint::of(other->getX509(), "sha-512");

    EXPECT_TRUE(CertificateFingerprint::matches(certificate->getX509(), {sha1}));
    EXPECT_FALSE(
        CertificateFingerprint::matches(certificate->getX509(), {certificate->getFingerprint(), wrong_sha512}));
    EXPECT_TRUE(CertificateFingerprint::matches(
        certificate->getX509(),
        {*CertificateFingerprint::of(other->getX509(), "sha-256"), certificate->getFingerprint()}));
}

TEST(CertificateFingerprintTest, NeverVerifiesWithMd5OrAnUnknownHash)
{
    const auto certificate = Certificate::generate();
    EXPECT_FALSE(CertificateFingerprint::of(certificate->getX509(), "md5"));
    EXPECT_FALSE(CertificateFingerprint::matches(certificate->getX509(), {{.algorithm = "md5", .value = "00"}}));
    EXPECT_FALSE(CertificateFingerprint::matches(certificate->getX509(), {{.algorithm = "sha-3", .value = "00"}}));
    EXPECT_FALSE(CertificateFingerprint::matches(certificate->getX509(), {}));
}

}  // namespace
}  // namespace endgate::rtc
