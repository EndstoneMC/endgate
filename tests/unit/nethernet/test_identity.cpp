#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "endgate/network/nethernet/identity.h"

namespace endgate::nethernet {
namespace {

using namespace std::chrono_literals;

const std::string ANSWER_SDP = "v=0\r\n"
                               "o=- 1 2 IN IP4 127.0.0.1\r\n"
                               "s=-\r\n"
                               "t=0 0\r\n"
                               "a=group:BUNDLE 0\r\n"
                               "a=ice-lite\r\n"
                               "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                               "c=IN IP4 0.0.0.0\r\n"
                               "a=ice-ufrag:wxyzWXYZ\r\n"
                               "a=ice-pwd:zyxwvutsrqponmlkjihgfedc\r\n"
                               "a=fingerprint:sha-256 11:22:33\r\n"
                               "a=setup:active\r\n"
                               "a=mid:0\r\n";

const std::chrono::system_clock::time_point NOW{std::chrono::seconds{1'800'000'000}};

PrivateKey generateKey(const char *curve)
{
    return {EVP_EC_gen(curve), EVP_PKEY_free};
}

std::string base64Decoded(std::string_view text)
{
    std::string decoded(text.size(), '\0');
    const auto length =
        EVP_DecodeBlock(reinterpret_cast<unsigned char *>(decoded.data()),
                        reinterpret_cast<const unsigned char *>(text.data()), static_cast<int>(text.size()));
    decoded.resize(static_cast<std::size_t>(length));
    while (!decoded.empty() && decoded.back() == '\0') {
        decoded.pop_back();
    }
    return decoded;
}

TEST(IdentityAssertionTest, SerialisesFingerprintsAsBdsDoes)
{
    const std::string sdp = "a=fingerprint:sha-256 BB:01\r\n"
                            "A=FINGERPRINT:sha-1 FF\n"
                            "a=fingerprint:sha-256 AA:02\r"
                            "a=fingerprint:broken\r\n"
                            "a=fingerprint:sha-256 \r\n";
    EXPECT_EQ(IdentityAssertion::fingerprintsOf(sdp),
              R"({"fingerprint":[{"algorithm":"sha-1","digest":"FF"},{"algorithm":"sha-256","digest":"AA:02"},)"
              R"({"algorithm":"sha-256","digest":"BB:01"}]})");
    EXPECT_EQ(IdentityAssertion::fingerprintsOf("v=0\r\n"), R"({"fingerprint":[]})");
}

TEST(IdentityAssertionTest, InjectsTheEnvelopeBeforeTheFirstMediaLine)
{
    const ServerIdentityAssertionGenerator generator(generateKey("P-384"), {.domain = "endgate.test"});
    const auto identity = IdentityAssertion::create(generator, ANSWER_SDP, NOW);
    ASSERT_TRUE(identity);
    const auto signed_sdp = identity->injectIntoSdp(ANSWER_SDP);

    const auto identity_at = signed_sdp.find("a=identity:");
    const auto media_at = signed_sdp.find("m=application");
    ASSERT_NE(identity_at, std::string::npos);
    EXPECT_EQ(signed_sdp.find("\r\n", identity_at) + 2, media_at);
    EXPECT_TRUE(signed_sdp.ends_with("a=mid:0\r\n"));
    EXPECT_EQ(IdentityAssertion::stripFromSdp(signed_sdp), ANSWER_SDP);

    const auto encoded = signed_sdp.substr(identity_at + 11, signed_sdp.find("\r\n", identity_at) - identity_at - 11);
    const auto envelope = base64Decoded(encoded);
    EXPECT_TRUE(envelope.starts_with(R"({"assertion":"{\"fingerprints\":\")")) << envelope;
    EXPECT_TRUE(envelope.ends_with(R"(","idp":{"domain":"endgate.test","protocol":"default"}})")) << envelope;
}

TEST(IdentityAssertionTest, SignsAsBdsDoesByDefault)
{
    const ServerIdentityAssertionGenerator generator(generateKey("P-384"), {});
    const auto identity = IdentityAssertion::extractFromSdp(
        IdentityAssertion::create(generator, ANSWER_SDP, NOW)->injectIntoSdp(ANSWER_SDP));
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity->getIdpDomain(), "self");
    const auto token = JsonWebToken::decode(identity->getToken().value_or(""));
    ASSERT_TRUE(token);
    EXPECT_NE(token->claims.find(R"("exp":1800000060,"iat":1800000000)"), std::string::npos) << token->claims;
}

TEST(IdentityAssertionTest, ReadsBackAndVerifiesItsOwnAssertion)
{
    const auto key = generateKey("P-384");
    const ServerIdentityAssertionGenerator generator(key, {.domain = "endgate.test"});
    const auto signed_sdp = IdentityAssertion::create(generator, ANSWER_SDP, NOW)->injectIntoSdp(ANSWER_SDP);

    const auto identity = IdentityAssertion::extractFromSdp(signed_sdp);
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity->getIdpDomain(), "endgate.test");
    EXPECT_EQ(identity->getIdpProtocol(), "default");
    EXPECT_EQ(identity->getFingerprints(), R"({"fingerprint":[{"algorithm":"sha-256","digest":"11:22:33"}]})");
    EXPECT_TRUE(identity->verifyFingerprints(key.get()));
    EXPECT_FALSE(identity->verifyFingerprints(generateKey("P-384").get()));

    auto tampered = signed_sdp;
    tampered.replace(tampered.find("11:22:33"), 8, "11:22:34");
    EXPECT_FALSE(IdentityAssertion::extractFromSdp(tampered)->verifyFingerprints(key.get()));

    const auto token = JsonWebToken::decode(identity->getToken().value_or(""));
    ASSERT_TRUE(token);
    EXPECT_EQ(token->algorithm, "ES384");
    EXPECT_TRUE(token->header.starts_with(R"({"alg":"ES384","x5u":"MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE)"))
        << token->header;
    EXPECT_TRUE(token->header.ends_with(R"("})")) << token->header;
    EXPECT_TRUE(token->verify(key.get()));
    EXPECT_NE(token->claims.find(R"("kty":"EC")"), std::string::npos);
    EXPECT_NE(token->claims.find(R"("crv":"P-384")"), std::string::npos);
    EXPECT_NE(token->claims.find(R"("iat":1800000000)"), std::string::npos);
    EXPECT_NE(token->claims.find(R"("exp":1800000060)"), std::string::npos);
    const auto cpk = token->publicKey();
    ASSERT_TRUE(cpk);
    EXPECT_EQ(EVP_PKEY_eq(cpk.get(), key.get()), 1);
}

TEST(IdentityAssertionTest, CanCarryTheKeyAsSubjectPublicKeyInfo)
{
    const auto key = generateKey("P-256");
    const ServerIdentityAssertionGenerator generator(
        key, {.domain = "endgate.test", .token_lifetime = 1h, .cpk_encoding = CpkEncoding::SubjectPublicKeyInfo});
    const auto identity = IdentityAssertion::extractFromSdp(
        IdentityAssertion::create(generator, ANSWER_SDP, NOW)->injectIntoSdp(ANSWER_SDP));
    ASSERT_TRUE(identity);
    const auto token = JsonWebToken::decode(identity->getToken().value_or(""));
    ASSERT_TRUE(token);
    EXPECT_EQ(token->algorithm, "ES256");
    EXPECT_NE(token->claims.find(R"("cpk":"MFkw)"), std::string::npos);
    EXPECT_NE(token->claims.find(R"("exp":1800003600)"), std::string::npos);
    const auto cpk = token->publicKey();
    ASSERT_TRUE(cpk);
    EXPECT_EQ(EVP_PKEY_eq(cpk.get(), key.get()), 1);
    EXPECT_TRUE(identity->verifyFingerprints(cpk.get()));
}

TEST(IdentityAssertionTest, RejectsBrokenEnvelopes)
{
    const auto with = [](std::string_view value) {
        return ANSWER_SDP + "a=identity:" + std::string(value) + "\r\n";
    };
    EXPECT_FALSE(IdentityAssertion::extractFromSdp(ANSWER_SDP));
    EXPECT_FALSE(IdentityAssertion::extractFromSdp(with("not base64!")));
    EXPECT_FALSE(IdentityAssertion::extractFromSdp(with("e30=")));
    EXPECT_FALSE(IdentityAssertion::extractFromSdp(with("eyJhc3NlcnRpb24iOjF9")));
    const auto *const envelope = "eyJhc3NlcnRpb24iOiJ4IiwiaWRwIjp7ImRvbWFpbiI6ImQiLCJwcm90b2NvbCI6InAifX0=";
    EXPECT_TRUE(IdentityAssertion::extractFromSdp(with(envelope)));
    EXPECT_FALSE(IdentityAssertion::extractFromSdp("v=0\r\na=identity:" + std::string(envelope) + "\r\n"));
    EXPECT_FALSE(IdentityAssertion::extractFromSdp(with(envelope))->getToken());
}

TEST(ClientIdentityAssertionVerifierTest, AcceptsAnOfferSignedUnderItsBase64DerCpk)
{
    const auto key = generateKey("P-384");
    const ServerIdentityAssertionGenerator generator(key, {.cpk_encoding = CpkEncoding::SubjectPublicKeyInfo});
    const auto offer = IdentityAssertion::create(generator, ANSWER_SDP, NOW)->injectIntoSdp(ANSWER_SDP);

    const auto token = ClientIdentityAssertionVerifier::verify(offer);
    ASSERT_TRUE(token);
    EXPECT_EQ(token->algorithm, "ES384");
    EXPECT_NE(token->claims.find(R"("cpk":"MHYw)"), std::string::npos) << token->claims;
}

TEST(ClientIdentityAssertionVerifierTest, RefusesWhatBdsRefuses)
{
    const ServerIdentityAssertionGenerator spki(generateKey("P-384"),
                                                {.cpk_encoding = CpkEncoding::SubjectPublicKeyInfo});
    const ServerIdentityAssertionGenerator jwk(generateKey("P-384"), {});
    const auto offer = IdentityAssertion::create(spki, ANSWER_SDP, NOW)->injectIntoSdp(ANSWER_SDP);

    EXPECT_FALSE(ClientIdentityAssertionVerifier::verify(ANSWER_SDP));
    EXPECT_FALSE(ClientIdentityAssertionVerifier::verify(
        IdentityAssertion::create(jwk, ANSWER_SDP, NOW)->injectIntoSdp(ANSWER_SDP)));
    auto tampered = offer;
    tampered.replace(tampered.find("11:22:33"), 8, "11:22:34");
    EXPECT_FALSE(ClientIdentityAssertionVerifier::verify(tampered));
    auto without_fingerprint = offer;
    without_fingerprint.erase(without_fingerprint.find("a=fingerprint:"),
                              std::string_view{"a=fingerprint:sha-256 11:22:33\r\n"}.size());
    EXPECT_FALSE(ClientIdentityAssertionVerifier::verify(without_fingerprint));
}

TEST(ServerIdentityAssertionGeneratorTest, KeepsItsKeyAcrossRestarts)
{
    const auto directory =
        std::filesystem::temp_directory_path() / ("endgate-identity-" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(directory);
    const auto path = directory / "keys" / "server_identity_key.pem";

    const auto created = ServerIdentityAssertionGenerator::loadOrCreateKey(path);
    ASSERT_TRUE(created);
    EXPECT_EQ(EVP_PKEY_get_bits(created.get()), 384);
#ifndef _WIN32
    EXPECT_EQ(std::filesystem::status(path).permissions() & std::filesystem::perms::all,
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
    EXPECT_FALSE(std::filesystem::exists(directory / "keys" / "server_identity_key.pem.tmp"));
    std::string first_line;
    std::getline(std::ifstream(path), first_line);
    EXPECT_EQ(first_line, "-----BEGIN EC PRIVATE KEY-----");

    const auto loaded = ServerIdentityAssertionGenerator::loadOrCreateKey(path);
    EXPECT_EQ(EVP_PKEY_eq(created.get(), loaded.get()), 1);

    std::ofstream(path, std::ios::trunc) << "garbage";
    EXPECT_THROW((void)ServerIdentityAssertionGenerator::loadOrCreateKey(path), std::runtime_error);
    std::filesystem::remove_all(directory);
}

TEST(ServerIdentityAssertionGeneratorTest, RefusesAKeyBdsWouldRefuse)
{
    const auto directory =
        std::filesystem::temp_directory_path() / ("endgate-identity-p256-" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "server_identity_key.pem";
    const auto key = generateKey("P-256");
    {
        const std::unique_ptr<BIO, decltype(&BIO_free)> file(BIO_new_file(path.string().c_str(), "w"), BIO_free);
        ASSERT_EQ(PEM_write_bio_PrivateKey(file.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr), 1);
    }
    EXPECT_THROW((void)ServerIdentityAssertionGenerator::loadOrCreateKey(path), std::runtime_error);
    std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace endgate::nethernet
