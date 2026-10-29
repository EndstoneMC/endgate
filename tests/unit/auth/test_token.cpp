#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include "endgate/auth/keys.h"
#include "endgate/auth/token.h"
#include "endgate/crypto/jose.h"
#include "endgate/network/eventloop.h"
#include "endgate/network/http.h"

namespace endgate {
namespace {

using namespace std::chrono_literals;

constexpr auto AWAITABLE = asio::as_tuple(asio::use_awaitable);
constexpr std::string_view ISSUER = "https://authorization.test/";

std::string base64UrlOf(std::string_view text)
{
    return base64UrlEncode(std::as_bytes(std::span{text.data(), text.size()}));
}

std::string base64UrlOf(const BIGNUM *number)
{
    std::string bytes(static_cast<std::size_t>(BN_num_bytes(number)), '\0');
    BN_bn2bin(number, reinterpret_cast<unsigned char *>(bytes.data()));
    return base64UrlOf(bytes);
}

std::string jwkOf(EVP_PKEY *key, std::string_view kid)
{
    BIGNUM *n = nullptr;
    BIGNUM *e = nullptr;
    EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_N, &n);
    EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_RSA_E, &e);
    const std::unique_ptr<BIGNUM, decltype(&BN_free)> owned_n(n, BN_free);
    const std::unique_ptr<BIGNUM, decltype(&BN_free)> owned_e(e, BN_free);
    return std::format(R"({{"kty":"RSA","use":"sig","kid":"{}","n":"{}","e":"{}"}})", kid, base64UrlOf(n),
                       base64UrlOf(e));
}

std::string signRs256(EVP_PKEY *key, std::string_view kid, std::string_view claims)
{
    const auto input =
        base64UrlOf(std::format(R"({{"alg":"RS256","kid":"{}","typ":"JWT"}})", kid)) + "." + base64UrlOf(claims);
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    std::size_t length = 0;
    EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key);
    EVP_DigestSign(context.get(), nullptr, &length, reinterpret_cast<const unsigned char *>(input.data()),
                   input.size());
    std::string signature(length, '\0');
    EVP_DigestSign(context.get(), reinterpret_cast<unsigned char *>(signature.data()), &length,
                   reinterpret_cast<const unsigned char *>(input.data()), input.size());
    return input + "." + base64UrlOf(signature);
}

std::string httpDate(std::chrono::system_clock::time_point time)
{
    const auto seconds = std::chrono::system_clock::to_time_t(time);
    std::tm parts{};
    ::gmtime_r(&seconds, &parts);
    std::array<char, 64> text{};
    const auto length = std::strftime(text.data(), text.size(), "%a, %d %b %Y %H:%M:%S GMT", &parts);
    return {text.data(), length};
}

class MinecraftServices {
public:
    MinecraftServices()
        : key_(EVP_RSA_gen(2048), EVP_PKEY_free), other_(EVP_RSA_gen(2048), EVP_PKEY_free),
          acceptor_(loop_.getExecutor(), {asio::ip::make_address("127.0.0.1"), 0})
    {
        asio::co_spawn(loop_.getExecutor(), serve(), asio::detached);
    }

    MinecraftServices(const MinecraftServices &) = delete;
    MinecraftServices &operator=(const MinecraftServices &) = delete;
    MinecraftServices(MinecraftServices &&) = delete;
    MinecraftServices &operator=(MinecraftServices &&) = delete;

    ~MinecraftServices()
    {
        asio::post(loop_.getExecutor(), asio::use_future([this] { acceptor_.close(); })).wait();
    }

    [[nodiscard]] std::string uri(std::string_view path) const
    {
        return std::format("http://127.0.0.1:{}{}", acceptor_.local_endpoint().port(), path);
    }

    [[nodiscard]] std::shared_ptr<MinecraftServiceKeyManager> keys(std::string_view discovery = "/discovery")
    {
        return std::make_shared<MinecraftServiceKeyManager>(HttpClient(loop_, {}), "Endgate/test", uri(discovery));
    }

    [[nodiscard]] EVP_PKEY *key() const { return key_.get(); }
    [[nodiscard]] EVP_PKEY *other() const { return other_.get(); }
    [[nodiscard]] int keySetRequests() const { return key_set_requests_; }

private:
    Awaitable<void> serve()
    {
        while (acceptor_.is_open()) {
            auto [error, socket] = co_await acceptor_.async_accept(AWAITABLE);
            if (error) {
                co_return;
            }
            std::string request;
            const auto [read_error, read] =
                co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", AWAITABLE);
            if (read_error) {
                continue;
            }
            const auto path = request.substr(4, request.find(' ', 4) - 4);
            const auto [write_error, written] =
                co_await asio::async_write(socket, asio::buffer(respond(path)), AWAITABLE);
        }
    }

    std::string respond(std::string_view path)
    {
        if (path == "/discovery") {
            return ok(std::format(R"({{"result":{{"serviceEnvironments":{{"auth":{{"prod":{{"serviceUri":"{}",)"
                                  R"("issuer":"{}"}}}}}},"supportedEnvironments":{{}}}}}})",
                                  uri("/auth/"), ISSUER),
                      "");
        }
        if (path == "/auth/.well-known/openid-configuration") {
            return ok(std::format(R"({{"jwks_uri":"{}","issuer":"{}"}})", uri("/keys"), ISSUER),
                      std::format("Date: {}\r\n", httpDate(std::chrono::system_clock::now() + 2h)));
        }
        if (path == "/keys") {
            ++key_set_requests_;
            const auto body = std::format(R"({{"keys":[{},{},{},{}]}})", jwkOf(key_.get(), "primary"),
                                          R"({"kty":"RSA","n":"AQAB","e":"AQAB"})", jwkOf(other_.get(), "primary"),
                                          R"({"kty":"oct","kid":"secret","k":"AA"})");
            return std::format("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n"
                               "\r\n{:x}\r\n{}\r\n{:x}\r\n{}\r\n0\r\n\r\n",
                               body.size() / 2, body.substr(0, body.size() / 2), body.size() - body.size() / 2,
                               body.substr(body.size() / 2));
        }
        return "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
    }

    static std::string ok(std::string_view body, std::string_view headers)
    {
        return std::format("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n{}Content-Length: {}\r\n\r\n{}",
                           headers, body.size(), body);
    }

    PrivateKey key_;
    PrivateKey other_;
    EventLoop loop_;
    asio::ip::tcp::acceptor acceptor_;
    std::atomic<int> key_set_requests_{0};
};

std::string claims(std::chrono::system_clock::time_point expiry, std::string_view audience,
                   std::string_view issuer = ISSUER)
{
    return std::format(R"({{"aud":{},"iss":"{}","exp":{},"xid":"2535400000000000","cpk":"MHYw"}})", audience, issuer,
                       std::chrono::duration_cast<std::chrono::seconds>(expiry.time_since_epoch()).count());
}

TEST(MinecraftServiceKeyManagerTest, FetchesTheKeysAndTheServiceClock)
{
    MinecraftServices services;
    const auto keys = services.keys();
    keys->start();

    ASSERT_TRUE(keys->hasKeys());
    const auto primary = keys->getKey("primary");
    ASSERT_TRUE(primary);
    EXPECT_EQ(EVP_PKEY_eq(primary.get(), services.key()), 1);
    EXPECT_FALSE(keys->getKey("secret"));
    EXPECT_EQ(keys->getIssuer(), ISSUER);
    const auto offset = keys->now() - std::chrono::system_clock::now();
    EXPECT_GT(offset, 2h - 5s);
    EXPECT_LT(offset, 2h + 5s);
    keys->shutdown();
}

TEST(MinecraftServiceKeyManagerTest, HasNoKeysWhenTheServiceIsDown)
{
    MinecraftServices services;
    const auto keys = services.keys("/down");
    keys->start();
    EXPECT_FALSE(keys->hasKeys());
    const auto token = JsonWebToken::decode(
        signRs256(services.key(), "primary",
                  claims(std::chrono::system_clock::now() + 1h, R"("api://auth-minecraft-services/multiplayer")")));
    ASSERT_TRUE(token);
    EXPECT_EQ(GameServerToken::validate(*token, *keys).error(), DisconnectFailReason::HostWithoutKeys);
    keys->shutdown();
}

TEST(GameServerTokenTest, ValidatesAsBdsDoes)
{
    constexpr std::string_view multiplayer = R"("api://auth-minecraft-services/multiplayer")";

    MinecraftServices services;
    const auto keys = services.keys();
    keys->start();
    const auto service_now = keys->now();
    const auto validate = [&keys](const std::string &token) {
        return GameServerToken::validate(JsonWebToken::decode(token).value(), *keys);
    };

    const auto valid = validate(signRs256(services.key(), "primary", claims(service_now + 1min, multiplayer)));
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->getXuid(), "2535400000000000");
    EXPECT_EQ(valid->getClientPublicKey(), "MHYw");
    EXPECT_TRUE(
        validate(signRs256(services.key(), "primary",
                           claims(service_now + 1min, R"(["other","api://auth-minecraft-services/multiplayer"])"))));

    const auto past_by_local_time = std::chrono::system_clock::now() + 1h;
    EXPECT_EQ(validate(signRs256(services.key(), "primary", claims(past_by_local_time, multiplayer))).error(),
              DisconnectFailReason::ExpiredToken);
    EXPECT_EQ(validate(signRs256(services.other(), "primary", claims(service_now + 1min, multiplayer))).error(),
              DisconnectFailReason::NotAuthenticated);
    EXPECT_EQ(validate(signRs256(services.key(), "unknown", claims(service_now + 1min, multiplayer))).error(),
              DisconnectFailReason::NotAuthenticated);
    EXPECT_EQ(validate(signRs256(services.key(), "primary", claims(service_now + 1min, R"("other")"))).error(),
              DisconnectFailReason::NotAuthenticated);
    EXPECT_EQ(
        validate(signRs256(services.key(), "primary", claims(service_now + 1min, R"("other")", "https://dev.test/")))
            .error(),
        DisconnectFailReason::DiscoveryEnvironmentMismatch);
    EXPECT_EQ(validate(signRs256(services.key(), "primary", R"({"aud":"x"})")).error(),
              DisconnectFailReason::NotAuthenticated);
    EXPECT_EQ(services.keySetRequests(), 1);
    keys->shutdown();
}

}  // namespace
}  // namespace endgate
