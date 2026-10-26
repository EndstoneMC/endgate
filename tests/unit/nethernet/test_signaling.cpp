#include <unistd.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ssl.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>
#include <openssl/pem.h>

#include "endgate/network/nethernet/signaling.h"
#include "endgate/network/rtc/certificate.h"

namespace endgate::nethernet {
namespace {

using namespace std::chrono_literals;

constexpr auto Awaitable = asio::as_tuple(asio::use_awaitable);
constexpr std::string_view Offer = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n";

class RecordingHandler : public JoinHandler {
public:
    ServerPing getServerPing() override
    {
        return {ServerPing::Version{924, "1.26.50"}, ServerPing::Players{3, 20}, "Endgate", "Bedrock level",
                GameType::Survival};
    }

    void onJoin(std::string network_id, std::string offer, JoinResponse response) override
    {
        const std::lock_guard lock(mutex);
        joins.emplace_back(std::move(network_id), std::move(offer));
        behaviour(response);
    }

    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> joins;
    std::function<void(const JoinResponse &)> behaviour = [](const JoinResponse &response) {
        response.answer("v=0\r\nanswer\r\n");
    };
};

std::string post(std::string_view path, std::string_view body)
{
    return std::format("POST {} HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/sdp\r\nUser-Agent: "
                       "libhttpclient/1.0.0.0\r\nContent-Length: {}\r\n\r\n{}",
                       path, body.size(), body);
}

class HttpSignalingServerTest : public testing::Test {
protected:
    void start(HttpSignalingServer::Config config, std::shared_ptr<asio::ssl::context> tls = nullptr)
    {
        server_ = HttpSignalingServer::create(io_context_, handler_, config, std::move(tls));
        server_->listen({asio::ip::make_address("127.0.0.1"), 0});
        endpoint_ = server_->getLocalEndpoint();
        thread_ = std::thread([this] { io_context_.run(); });
    }

    void SetUp() override
    {
        if (!server_) {
            start({.max_connections = 64,
                   .max_request_bytes = 65536,
                   .request_timeout = 300ms,
                   .write_timeout = 1s,
                   .join_timeout = 300ms});
        }
    }

    void TearDown() override
    {
        server_->close();
        work_.reset();
        io_context_.stop();
        thread_.join();
    }

    [[nodiscard]] std::string roundTrip(std::string request, std::chrono::milliseconds timeout = 3s) const
    {
        asio::io_context client;
        std::string response;
        asio::co_spawn(
            client,
            [&]() -> asio::awaitable<void> {
                asio::ip::tcp::socket socket(client);
                const auto [connect_error] = co_await socket.async_connect(endpoint_, Awaitable);
                if (connect_error) {
                    co_return;
                }
                co_await asio::async_write(socket, asio::buffer(request), Awaitable);
                std::array<char, 4096> chunk{};
                for (;;) {
                    const auto [error, received] = co_await socket.async_read_some(asio::buffer(chunk), Awaitable);
                    response.append(chunk.data(), received);
                    if (error) {
                        break;
                    }
                }
            },
            asio::detached);
        client.run_for(timeout);
        return response;
    }

    asio::io_context io_context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_{io_context_.get_executor()};
    RecordingHandler handler_;
    std::shared_ptr<HttpSignalingServer> server_;
    asio::ip::tcp::endpoint endpoint_;
    std::thread thread_;
};

TEST_F(HttpSignalingServerTest, AnswersThePingWithServerInfo)
{
    const auto response = roundTrip("GET /v1/join HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n")) << response;
    EXPECT_NE(response.find("Content-Type: application/json\r\n"), std::string::npos);
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\n"
                                   R"({"name":"Endgate","protocol":924,"version":"1.26.50","level":"Bedrock level",)"
                                   R"("players":3,"maxPlayers":20,"gameType":0})"))
        << response;
}

TEST_F(HttpSignalingServerTest, TradesAnOfferForAnAnswer)
{
    const auto response = roundTrip(post("/v1/join/9876543210123456789", Offer));
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n")) << response;
    EXPECT_NE(response.find("Content-Type: application/sdp\r\n"), std::string::npos);
    EXPECT_TRUE(response.ends_with("\r\n\r\nv=0\r\nanswer\r\n")) << response;
    const std::lock_guard lock(handler_.mutex);
    ASSERT_EQ(handler_.joins.size(), 1U);
    EXPECT_EQ(handler_.joins[0].first, "9876543210123456789");
    EXPECT_EQ(handler_.joins[0].second, Offer);
}

TEST_F(HttpSignalingServerTest, TakesAnOpaqueNetworkId)
{
    EXPECT_TRUE(roundTrip(post("/v1/join/0a1b-C2d3_e.f~g%20", Offer)).starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_F(HttpSignalingServerTest, AcceptsAnAnswerFromAnotherThreadLater)
{
    std::thread answering;
    handler_.behaviour = [&answering](const JoinResponse &response) {
        answering = std::thread([response] {
            std::this_thread::sleep_for(100ms);
            response.answer("later");
            response.answer("ignored");
        });
    };
    const auto response = roundTrip(post("/v1/join/1", Offer));
    answering.join();
    EXPECT_TRUE(response.ends_with("\r\n\r\nlater")) << response;
}

TEST_F(HttpSignalingServerTest, ReturnsASessionErrorAsTheBody)
{
    handler_.behaviour = [](const JoinResponse &response) {
        response.reject(37);
    };
    const auto response = roundTrip(post("/v1/join/1", Offer));
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n")) << response;
    EXPECT_NE(response.find("Content-Type: application/sdp\r\n"), std::string::npos);
    EXPECT_TRUE(response.ends_with("Content-Length: 2\r\nConnection: close\r\n\r\n37")) << response;
}

TEST_F(HttpSignalingServerTest, TimesOutAJoinNobodyAnswers)
{
    handler_.behaviour = [](const JoinResponse &) {
    };
    const auto started = std::chrono::steady_clock::now();
    const auto response = roundTrip(post("/v1/join/1", Offer));
    EXPECT_GE(std::chrono::steady_clock::now() - started, 300ms);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 504 Gateway Timeout\r\n")) << response;
    EXPECT_TRUE(response.ends_with("\r\n\r\nGateway Timeout")) << response;
}

TEST_F(HttpSignalingServerTest, RoutesOnlyTheTwoEndpoints)
{
    EXPECT_TRUE(roundTrip("GET /v2/join HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_TRUE(roundTrip("GET /v1/join/1 HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_TRUE(roundTrip(post("/v1/join", Offer)).starts_with("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_TRUE(roundTrip("PUT /v1/join HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 404 Not Found\r\n"));
}

TEST_F(HttpSignalingServerTest, RejectsABadNetworkIdOrAMissingOffer)
{
    for (const auto *path : {"/v1/join/", "/v1/join/a/b", "/v1/join/a?b", "/v1/join/a\"b"}) {
        EXPECT_TRUE(roundTrip(post(path, Offer)).starts_with("HTTP/1.1 400 Expected /v1/join/{networkId}\r\n")) << path;
    }
    EXPECT_TRUE(roundTrip(post("/v1/join/" + std::string(129, '1'), Offer)).starts_with("HTTP/1.1 400 "));
    const auto response = roundTrip(post("/v1/join/1", ""));
    EXPECT_TRUE(response.starts_with("HTTP/1.1 400 Missing SDP offer in request body\r\n")) << response;
    EXPECT_TRUE(response.ends_with("\r\n\r\nMissing SDP offer in request body"));
    const std::lock_guard lock(handler_.mutex);
    EXPECT_TRUE(handler_.joins.empty());
}

TEST_F(HttpSignalingServerTest, ReadsARequestArrivingInPieces)
{
    asio::io_context client;
    std::string response;
    asio::co_spawn(
        client,
        [&]() -> asio::awaitable<void> {
            asio::ip::tcp::socket socket(client);
            co_await socket.async_connect(endpoint_, Awaitable);
            const std::string head =
                "POST /v1/join/7 HTTP/1.1\r\ncOnTeNt-LeNgTh:  " + std::to_string(Offer.size()) + " \r\n\r\n";
            co_await asio::async_write(socket, asio::buffer(head), Awaitable);
            asio::steady_timer pause(client, 50ms);
            co_await pause.async_wait(Awaitable);
            co_await asio::async_write(socket, asio::buffer(Offer), Awaitable);
            std::array<char, 4096> chunk{};
            for (;;) {
                const auto [error, received] = co_await socket.async_read_some(asio::buffer(chunk), Awaitable);
                response.append(chunk.data(), received);
                if (error) {
                    break;
                }
            }
        },
        asio::detached);
    client.run_for(3s);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n")) << response;
    const std::lock_guard lock(handler_.mutex);
    ASSERT_EQ(handler_.joins.size(), 1U);
    EXPECT_EQ(handler_.joins[0].second, Offer);
}

TEST_F(HttpSignalingServerTest, DropsOversizeRequestsWithoutAnswering)
{
    EXPECT_EQ(roundTrip("POST /v1/join/1 HTTP/1.1\r\nContent-Length: 70000\r\n\r\nv=0"), "");
    EXPECT_EQ(roundTrip("GET /v1/join HTTP/1.1\r\nX-Padding: " + std::string(70000, 'a') + "\r\n\r\n"), "");
    EXPECT_EQ(roundTrip(std::string(70000, 'a')), "");
}

TEST_F(HttpSignalingServerTest, DropsMalformedRequestsWithoutAnswering)
{
    EXPECT_EQ(roundTrip("garbage\r\n\r\n"), "");
    EXPECT_EQ(roundTrip("get /v1/join HTTP/1.1\r\n\r\n"), "");
    EXPECT_EQ(roundTrip("GET v1/join HTTP/1.1\r\n\r\n"), "");
    EXPECT_EQ(roundTrip("POST /v1/join/1 HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nv=0"), "");
    EXPECT_EQ(roundTrip("POST /v1/join/1 HTTP/1.1\r\nContent-Length: -3\r\n\r\nv=0"), "");
    EXPECT_TRUE(roundTrip("GET /v1/join HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_F(HttpSignalingServerTest, ClosesAStalledConnectionWithoutWedgingTheOthers)
{
    asio::io_context client;
    asio::ip::tcp::socket stalled(client);
    stalled.connect(endpoint_);
    asio::write(stalled, asio::buffer(std::string_view{"POST /v1/join/1 HTTP/1.1\r\nContent-Length: 100\r\n\r\nv="}));
    const auto started = std::chrono::steady_clock::now();

    EXPECT_TRUE(roundTrip("GET /v1/join HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 250ms);

    std::array<char, 16> chunk{};
    asio::error_code error;
    const auto received = stalled.read_some(asio::buffer(chunk), error);
    EXPECT_EQ(received, 0U);
    EXPECT_TRUE(error);
    EXPECT_GE(std::chrono::steady_clock::now() - started, 250ms);
}

TEST_F(HttpSignalingServerTest, RefusesTlsAtOnceWhenNoCertificateIsConfigured)
{
    const std::string client_hello{"\x16\x03\x01\x00\x05\x01\x00\x00\x01\x00", 10};
    const auto started = std::chrono::steady_clock::now();
    const auto response = roundTrip(client_hello);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 250ms);
    EXPECT_EQ(response, std::string("\x15\x03\x01\x00\x02\x02\x28", 7));
    EXPECT_TRUE(roundTrip("GET /v1/join HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 200 OK\r\n"));
}

class TlsSignalingServerTest : public HttpSignalingServerTest {
protected:
    void SetUp() override
    {
        directory_ = std::filesystem::temp_directory_path() / ("endgate-tls-" + std::to_string(::getpid()));
        std::filesystem::create_directories(directory_);
        const auto certificate = rtc::Certificate::generate();
        const std::unique_ptr<BIO, decltype(&BIO_free)> chain(BIO_new_file((directory_ / "chain.pem").c_str(), "w"),
                                                              BIO_free);
        const std::unique_ptr<BIO, decltype(&BIO_free)> key(BIO_new_file((directory_ / "key.pem").c_str(), "w"),
                                                            BIO_free);
        PEM_write_bio_X509(chain.get(), certificate->getX509());
        PEM_write_bio_PrivateKey(key.get(), certificate->getPrivateKey(), nullptr, nullptr, 0, nullptr, nullptr);
        BIO_flush(chain.get());
        BIO_flush(key.get());
        start({.request_timeout = 300ms, .write_timeout = 1s, .join_timeout = 300ms},
              HttpSignalingServer::loadTls(directory_ / "chain.pem", directory_ / "key.pem"));
    }

    void TearDown() override
    {
        HttpSignalingServerTest::TearDown();
        std::filesystem::remove_all(directory_);
    }

    std::filesystem::path directory_;
};

TEST_F(TlsSignalingServerTest, ServesHttpsAndHttpOnOnePort)
{
    asio::io_context client;
    asio::ssl::context context(asio::ssl::context::tls_client);
    context.set_verify_mode(asio::ssl::verify_none);
    std::string response;
    asio::co_spawn(
        client,
        [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> stream(client, context);
            co_await stream.next_layer().async_connect(endpoint_, Awaitable);
            const auto [handshake_error] = co_await stream.async_handshake(asio::ssl::stream_base::client, Awaitable);
            if (handshake_error) {
                co_return;
            }
            const auto request = post("/v1/join/42", Offer);
            co_await asio::async_write(stream, asio::buffer(request), Awaitable);
            std::array<char, 4096> chunk{};
            for (;;) {
                const auto [error, received] = co_await stream.async_read_some(asio::buffer(chunk), Awaitable);
                response.append(chunk.data(), received);
                if (error) {
                    break;
                }
            }
        },
        asio::detached);
    client.run_for(3s);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n")) << response;
    EXPECT_TRUE(response.ends_with("\r\n\r\nv=0\r\nanswer\r\n")) << response;
    EXPECT_TRUE(roundTrip("GET /v1/join HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_F(TlsSignalingServerTest, ClosesAStalledHandshake)
{
    const auto started = std::chrono::steady_clock::now();
    const auto response = roundTrip(std::string("\x16\x03\x01\x02\x00", 5));
    EXPECT_EQ(response, "");
    EXPECT_GE(std::chrono::steady_clock::now() - started, 250ms);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
}

class CappedSignalingServerTest : public HttpSignalingServerTest {
protected:
    void SetUp() override
    {
        start({.max_connections = 2, .request_timeout = 300ms, .write_timeout = 1s, .join_timeout = 300ms});
    }
};

TEST_F(CappedSignalingServerTest, RefusesConnectionsOverTheCapUntilOthersClose)
{
    asio::io_context client;
    asio::ip::tcp::socket first(client);
    asio::ip::tcp::socket second(client);
    first.connect(endpoint_);
    second.connect(endpoint_);
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(roundTrip("GET /v1/join HTTP/1.1\r\n\r\n"), "");
    std::this_thread::sleep_for(400ms);
    EXPECT_TRUE(roundTrip("GET /v1/join HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_F(HttpSignalingServerTest, StopsAcceptingWhenClosed)
{
    server_->close();
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(roundTrip("GET /v1/join HTTP/1.1\r\n\r\n", 500ms), "");
}

}  // namespace
}  // namespace endgate::nethernet
