#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <glaze/json.hpp>
#include <gtest/gtest.h>
#include <openssl/ec.h>
#include <openssl/evp.h>

#include "endgate/connection/backend.h"
#include "endgate/connection/player.h"
#include "endgate/crypto/jose.h"
#include "endgate/network/nethernet/identity.h"

namespace endgate {
namespace {

using namespace std::chrono_literals;

const std::chrono::system_clock::time_point NOW{std::chrono::seconds{1'800'000'000}};

struct Info {
    int version = 0;
    std::string address;

    struct glaze {
        using T = Info;
        static constexpr auto value = glz::object("version", &T::version, "address", &T::address);
    };
};

struct ForwardedClaims {
    std::string aud;
    std::string iss;
    std::string xid;
    std::string xname;
    std::string mid;
    std::string cpk;
    std::int64_t exp = 0;
    std::int64_t iat = 0;
    Info info;

    struct glaze {
        using T = ForwardedClaims;
        static constexpr auto value =
            glz::object("aud", &T::aud, "iss", &T::iss, "xid", &T::xid, "xname", &T::xname, "mid", &T::mid, "cpk",
                        &T::cpk, "exp", &T::exp, "iat", &T::iat, "endgate:player_info", &T::info);
    };
};

nethernet::ServerIdentityAssertionGenerator makeIdentity()
{
    return {PrivateKey{EVP_EC_gen("P-384"), EVP_PKEY_free}, {}};
}

GameProfile makeProfile()
{
    return GameProfile{.xuid = "2535400000000042",
                       .name = "Alex",
                       .identity_claims = R"({"aud":"api://auth-minecraft-services/multiplayer",)"
                                          R"("iss":"https://issuer.test","xid":"2535400000000042",)"
                                          R"("xname":"Alex","mid":"abc","cpk":"clientkey","exp":1,"iat":2})"};
}

}  // namespace

TEST(ForwardingTokenTest, HeaderHoldsOnlyTheAlgorithmAndTheKeyId)
{
    const auto identity = makeIdentity();
    const auto token = JsonWebToken::decode(
        ServerConnection::createForwardingData(identity, "203.0.113.5", makeProfile(), NOW).value());
    ASSERT_TRUE(token);
    EXPECT_EQ(token->header, R"({"alg":"ES384","kid":")" + identity.getPublicKey() + R"("})");
    EXPECT_TRUE(token->x5u.empty());
}

TEST(ForwardingTokenTest, KeepsThePlayersClaimsAndReplacesCpkExpAndIat)
{
    const auto identity = makeIdentity();
    const auto token = JsonWebToken::decode(
        ServerConnection::createForwardingData(identity, "203.0.113.5", makeProfile(), NOW).value());
    ASSERT_TRUE(token);
    ForwardedClaims claims;
    ASSERT_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(claims, token->claims)) << token->claims;
    EXPECT_EQ(claims.aud, "api://auth-minecraft-services/multiplayer");
    EXPECT_EQ(claims.iss, "https://issuer.test");
    EXPECT_EQ(claims.xid, "2535400000000042");
    EXPECT_EQ(claims.xname, "Alex");
    EXPECT_EQ(claims.mid, "abc");
    EXPECT_EQ(claims.cpk, identity.getPublicKey());
    EXPECT_EQ(claims.iat, 1'800'000'000);
    EXPECT_EQ(claims.exp, 1'800'003'600);
}

TEST(ForwardingTokenTest, CarriesThePlayerInfoVersionAndAddress)
{
    const auto identity = makeIdentity();
    const auto token = JsonWebToken::decode(
        ServerConnection::createForwardingData(identity, "203.0.113.5", makeProfile(), NOW).value());
    ASSERT_TRUE(token);
    ForwardedClaims claims;
    ASSERT_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(claims, token->claims));
    EXPECT_EQ(claims.info.version, 1);
    EXPECT_EQ(claims.info.address, "203.0.113.5");
    EXPECT_NE(token->claims.find(R"("endgate:player_info":{"version":1,"address":"203.0.113.5"})"), std::string::npos)
        << token->claims;
}

TEST(ForwardingTokenTest, VerifiesUnderTheKeyIdAndNoOther)
{
    const auto identity = makeIdentity();
    const auto token = JsonWebToken::decode(
        ServerConnection::createForwardingData(identity, "203.0.113.5", makeProfile(), NOW).value());
    ASSERT_TRUE(token);
    const auto key = decodeSubjectPublicKeyInfo(token->key_id);
    ASSERT_TRUE(key);
    EXPECT_TRUE(token->verify(key.get()));
    EXPECT_EQ(EVP_PKEY_eq(key.get(), identity.getKey()), 1);
    EXPECT_FALSE(token->verify(makeIdentity().getKey()));
    const auto cpk = token->publicKey();
    ASSERT_TRUE(cpk);
    EXPECT_EQ(EVP_PKEY_eq(cpk.get(), identity.getKey()), 1);
}

TEST(ForwardingTokenTest, RefusesAProfileWithoutClaims)
{
    const auto identity = makeIdentity();
    EXPECT_FALSE(ServerConnection::createForwardingData(identity, "203.0.113.5", GameProfile{}, NOW));
}

TEST(ForwardingTokenTest, OnlyTheForwardingTokenCarriesAudAndKid)
{
    const auto identity = makeIdentity();
    const auto assertion = nethernet::IdentityAssertion::create(
        identity, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=fingerprint:sha-256 11:22:33\r\n", NOW);
    ASSERT_TRUE(assertion);
    const auto identity_token = JsonWebToken::decode(assertion->getToken().value_or(""));
    ASSERT_TRUE(identity_token);
    for (const auto &text : {identity_token->header, identity_token->claims}) {
        EXPECT_EQ(text.find("\"aud\""), std::string::npos) << text;
        EXPECT_EQ(text.find("\"kid\""), std::string::npos) << text;
    }

    const auto forwarding = JsonWebToken::decode(
        ServerConnection::createForwardingData(identity, "203.0.113.5", makeProfile(), NOW).value());
    ASSERT_TRUE(forwarding);
    EXPECT_NE(forwarding->header.find("\"kid\""), std::string::npos);
    EXPECT_NE(forwarding->claims.find("\"aud\""), std::string::npos);
}

}  // namespace endgate
