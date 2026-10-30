#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include "endgate/connection/login.h"
#include "endgate/connection/player.h"
#include "endgate/crypto/jose.h"
#include "support.h"

namespace endgate {
namespace {

std::string publicKeyOf(EVP_PKEY *key)
{
    unsigned char *der = nullptr;
    const auto length = i2d_PUBKEY(key, &der);
    auto text = base64Encode(std::as_bytes(std::span{der, static_cast<std::size_t>(length)}));
    OPENSSL_free(der);
    return text;
}

std::string signedToken(EVP_PKEY *key, std::string_view claims)
{
    const auto header = R"({"alg":"ES384","x5u":")" + publicKeyOf(key) + R"("})";
    const auto input = base64UrlEncode(std::as_bytes(std::span{header})) + "." +
                       base64UrlEncode(std::as_bytes(std::span{claims.data(), claims.size()}));
    return input + "." + *signJws(key, input);
}

std::string longString(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>(text.size() >> (8 * i)));
    }
    return out + std::string(text);
}

std::string selfSignedRequest(EVP_PKEY *key, std::string_view claims, std::string_view client_data)
{
    const auto info = R"({"AuthenticationType":2,"Token":")" + signedToken(key, claims) + R"("})";
    return longString(info) + longString(signedToken(key, client_data));
}

class LoginTest : public testing::Test {
protected:
    [[nodiscard]] std::shared_ptr<TestConnection> connect()
    {
        return std::make_shared<TestConnection>(node_.endpoint(), "127.0.0.1:1", Direction::Serverbound);
    }

    [[nodiscard]] std::shared_ptr<Player> makePlayer(const std::shared_ptr<TestConnection> &connection,
                                                     std::string name, std::string_view xuid = {})
    {
        const auto id =
            xuid.empty() ? UuidUtils::generateOfflinePlayerUuid(name) : UuidUtils::generateXuidPlayerUuid(xuid);
        return std::make_shared<Player>(server_, connection,
                                        GameProfile{.id = id, .xuid = std::string(xuid), .name = std::move(name)});
    }

    ProxyServer server_;
    Registry registry_;
    Node node_{server_, registry_};
};

TEST_F(LoginTest, OfflineModeLogsInASelfSignedClientWithoutServiceKeys)
{
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_EC_gen("P-384"), EVP_PKEY_free);
    const auto cpk = publicKeyOf(key.get());
    const auto request = selfSignedRequest(key.get(), R"({"cpk":")" + cpk + R"(","iat":1})",
                                           R"({"ThirdPartyName":"Steve","x5u":")" + cpk + R"("})");

    ASSERT_EQ(server_.getMinecraftServiceKeyManager(), nullptr);
    const auto connection = connect();
    const auto protocol = connection->getProtocolVersion().getProtocol();
    node_.call([&] {
        InitialLoginSessionHandler handler(server_, connection, std::nullopt);
        EXPECT_TRUE(handler.handle(bp::LoginPacket{.client_network_version = protocol, .connection_request = request}));
        return 0;
    });

    EXPECT_EQ(connection->sent(), 2U);
    EXPECT_TRUE(connection->closed());
}

TEST_F(LoginTest, OfflineModeRejectsClientDataSignedByAnotherKey)
{
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_EC_gen("P-384"), EVP_PKEY_free);
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> other(EVP_EC_gen("P-384"), EVP_PKEY_free);
    const auto cpk = publicKeyOf(key.get());
    const auto info = R"({"AuthenticationType":2,"Token":")" +
                      signedToken(key.get(), R"({"cpk":")" + cpk + R"(","iat":1})") + R"("})";
    const auto request = longString(info) + longString(signedToken(other.get(), R"({"ThirdPartyName":"Steve"})"));

    const auto connection = connect();
    const auto protocol = connection->getProtocolVersion().getProtocol();
    node_.call([&] {
        InitialLoginSessionHandler handler(server_, connection, std::nullopt);
        EXPECT_TRUE(handler.handle(bp::LoginPacket{.client_network_version = protocol, .connection_request = request}));
        return 0;
    });

    EXPECT_EQ(connection->sent(), 1U);
    EXPECT_TRUE(connection->closed());
}

TEST_F(LoginTest, AConnectionCannotBeRegisteredTwiceUnderOneNameOrId)
{
    const auto first = makePlayer(connect(), "Steve", "2535428388216482");
    const auto same_name = makePlayer(connect(), "STEVE", "2535428388216483");
    const auto same_id = makePlayer(connect(), "Alex", "2535428388216482");
    const auto other = makePlayer(connect(), "Alex", "2535428388216484");

    EXPECT_TRUE(server_.canRegisterConnection(*first));
    EXPECT_TRUE(server_.registerConnection(first));
    EXPECT_FALSE(server_.canRegisterConnection(*same_name));
    EXPECT_FALSE(server_.registerConnection(same_name));
    EXPECT_FALSE(server_.canRegisterConnection(*same_id));
    EXPECT_FALSE(server_.registerConnection(same_id));
    EXPECT_TRUE(server_.canRegisterConnection(*other));
}

TEST_F(LoginTest, UnregisteringFreesTheNameAndIdOnlyForTheRegisteredPlayer)
{
    const auto first = makePlayer(connect(), "Steve", "2535428388216482");
    const auto second = makePlayer(connect(), "Steve", "2535428388216482");

    EXPECT_TRUE(server_.registerConnection(first));
    server_.unregisterConnection(*second);
    EXPECT_FALSE(server_.canRegisterConnection(*second));
    server_.unregisterConnection(*first);
    EXPECT_TRUE(server_.registerConnection(second));
}

}  // namespace
}  // namespace endgate
