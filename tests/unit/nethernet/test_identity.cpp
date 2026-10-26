#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <openssl/ec.h>
#include <openssl/evp.h>

#include "endgate/network/nethernet/identity.h"

namespace endgate::nethernet {
namespace {

using namespace std::chrono_literals;

const std::string AnswerSdp = "v=0\r\n"
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

const std::chrono::system_clock::time_point Now{std::chrono::seconds{1'800'000'000}};

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
    const auto identity = IdentityAssertion::create(generator, AnswerSdp, Now);
    ASSERT_TRUE(identity);
    const auto signed_sdp = identity->injectIntoSdp(AnswerSdp);

    const auto identity_at = signed_sdp.find("a=identity:");
    const auto media_at = signed_sdp.find("m=application");
    ASSERT_NE(identity_at, std::string::npos);
    EXPECT_EQ(signed_sdp.find("\r\n", identity_at) + 2, media_at);
    EXPECT_TRUE(signed_sdp.ends_with("a=mid:0\r\n"));
    EXPECT_EQ(IdentityAssertion::stripFromSdp(signed_sdp), AnswerSdp);

    const auto encoded = signed_sdp.substr(identity_at + 11, signed_sdp.find("\r\n", identity_at) - identity_at - 11);
    const auto envelope = base64Decoded(encoded);
    EXPECT_TRUE(envelope.starts_with(R"({"assertion":"{\"fingerprints\":\")")) << envelope;
    EXPECT_TRUE(envelope.ends_with(R"(","idp":{"domain":"endgate.test","protocol":"default"}})")) << envelope;
}

TEST(IdentityAssertionTest, ReadsBackAndVerifiesItsOwnAssertion)
{
    const auto key = generateKey("P-384");
    const ServerIdentityAssertionGenerator generator(key, {.domain = "endgate.test"});
    const auto signed_sdp = IdentityAssertion::create(generator, AnswerSdp, Now)->injectIntoSdp(AnswerSdp);

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
    EXPECT_EQ(token->header, R"({"alg":"ES384","typ":"JWT"})");
    EXPECT_TRUE(token->verify(key.get()));
    EXPECT_NE(token->claims.find(R"("kty":"EC")"), std::string::npos);
    EXPECT_NE(token->claims.find(R"("crv":"P-384")"), std::string::npos);
    EXPECT_NE(token->claims.find(R"("iat":1800000000)"), std::string::npos);
    EXPECT_NE(token->claims.find(R"("exp":1802592000)"), std::string::npos);
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
        IdentityAssertion::create(generator, AnswerSdp, Now)->injectIntoSdp(AnswerSdp));
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

TEST(IdentityAssertionTest, VerifiesTheRfc7515Es256Example)
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

TEST(IdentityAssertionTest, RejectsBrokenEnvelopes)
{
    const auto with = [](std::string_view value) {
        return AnswerSdp + "a=identity:" + std::string(value) + "\r\n";
    };
    EXPECT_FALSE(IdentityAssertion::extractFromSdp(AnswerSdp));
    EXPECT_FALSE(IdentityAssertion::extractFromSdp(with("not base64!")));
    EXPECT_FALSE(IdentityAssertion::extractFromSdp(with("e30=")));
    EXPECT_FALSE(IdentityAssertion::extractFromSdp(with("eyJhc3NlcnRpb24iOjF9")));
    const auto *const envelope = "eyJhc3NlcnRpb24iOiJ4IiwiaWRwIjp7ImRvbWFpbiI6ImQiLCJwcm90b2NvbCI6InAifX0=";
    EXPECT_TRUE(IdentityAssertion::extractFromSdp(with(envelope)));
    EXPECT_FALSE(IdentityAssertion::extractFromSdp("v=0\r\na=identity:" + std::string(envelope) + "\r\n"));
    EXPECT_FALSE(IdentityAssertion::extractFromSdp(with(envelope))->getToken());
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

TEST(ServerIdentityAssertionGeneratorTest, KeepsItsKeyAcrossRestarts)
{
    const auto directory = std::filesystem::temp_directory_path() / ("endgate-identity-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    const auto path = directory / "nested" / "identity.pem";

    const auto created = ServerIdentityAssertionGenerator::loadOrCreateKey(path);
    ASSERT_TRUE(created);
    EXPECT_EQ(EVP_PKEY_get_bits(created.get()), 384);
    EXPECT_EQ(std::filesystem::status(path).permissions() & std::filesystem::perms::all,
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    const auto loaded = ServerIdentityAssertionGenerator::loadOrCreateKey(path);
    EXPECT_EQ(EVP_PKEY_eq(created.get(), loaded.get()), 1);

    std::ofstream(path, std::ios::trunc) << "garbage";
    EXPECT_THROW((void)ServerIdentityAssertionGenerator::loadOrCreateKey(path), std::runtime_error);
    std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace endgate::nethernet
