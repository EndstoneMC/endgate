#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <gtest/gtest.h>
#include <openssl/evp.h>

#include "endgate/network/eventloop.h"
#include "endgate/network/nethernet/client.h"
#include "endgate/network/nethernet/identity.h"
#include "endgate/network/nethernet/signaling.h"
#include "endgate/network/rtc/certificate.h"
#include "endgate/network/rtc/description.h"

namespace endgate::nethernet {
namespace {

using namespace std::chrono_literals;

class StubHandler : public JoinHandler {
public:
    StubHandler()
        : generator_(std::make_shared<ServerIdentityAssertionGenerator>(PrivateKey{EVP_EC_gen("P-384"), EVP_PKEY_free},
                                                                        ServerIdentityAssertionGenerator::Config{}))
    {
    }

    void rejectWith(std::uint32_t error) { reject_ = error; }

    Awaitable<ServerPing> getServerPing(const PingRequest &) override
    {
        co_return ServerPing{ServerPing::Version{800, "1.26.52"}, ServerPing::Players{3, 10}, "Endgate Test", "world",
                             GameType::Creative};
    }

    void onJoin(JoinRequest request, JoinResponse response) override
    {
        if (reject_) {
            response.reject(*reject_);
            return;
        }
        const auto offer = rtc::Description::parse(IdentityAssertion::stripFromSdp(request.offer));
        if (!offer) {
            response.reject(static_cast<std::uint32_t>(ESessionError::FailedToSetRemoteDescription));
            return;
        }
        const std::array addresses{rtc::CandidateAddress{.address = {asio::ip::make_address("127.0.0.1"), 19132}}};
        auto sdp =
            rtc::Description::answer(*offer, rtc::IceCredentials::generate(), certificate_->getFingerprint(), addresses)
                .generate(1);
        const auto identity = IdentityAssertion::create(*generator_, sdp, std::chrono::system_clock::now());
        response.answer(identity ? identity->injectIntoSdp(sdp) : sdp);
    }

private:
    std::shared_ptr<const rtc::Certificate> certificate_{rtc::Certificate::generate()};
    std::shared_ptr<ServerIdentityAssertionGenerator> generator_;
    std::optional<std::uint32_t> reject_;
};

class SignalingClientTest : public testing::Test {
protected:
    void listen()
    {
        server_ = HttpSignalingServer::create(loop_, handler_, {}, nullptr);
        server_->listen({asio::ip::make_address("127.0.0.1"), 0});
        server_->start();
        port_ = server_->getLocalEndpoint().port();
    }

    void TearDown() override
    {
        if (server_) {
            server_->close();
        }
    }

    SignalingClient::Config config_{.connect_timeout = 2s, .request_timeout = 2s, .tls_handshake_timeout = 500ms};
    asio::io_context io_;
    StubHandler handler_;
    std::shared_ptr<HttpSignalingServer> server_;
    std::uint16_t port_{0};
    EventLoop loop_;
};

TEST_F(SignalingClientTest, ProbeReportsTheServerAndItsPing)
{
    listen();
    std::optional<SignalingClient::Probe> probe;
    asio::co_spawn(io_, SignalingClient::probeNetherNet(io_, "127.0.0.1", port_, config_),
                   [&](const std::exception_ptr &, SignalingClient::Probe result) {
                       probe = std::move(result);
                       io_.stop();
                   });
    io_.run();

    ASSERT_TRUE(probe);
    EXPECT_TRUE(probe->supported);
    ASSERT_TRUE(probe->ping);
    EXPECT_EQ(probe->ping->getVersion().getProtocol(), 800);
    EXPECT_EQ(probe->ping->getDescription(), "Endgate Test");
    EXPECT_EQ(probe->ping->getPlayers().getMax(), 10);
    EXPECT_EQ(probe->ping->getGameType(), GameType::Creative);
}

TEST_F(SignalingClientTest, RequestAnswerReturnsAVerifiedAnswer)
{
    listen();
    const auto offer =
        rtc::Description::offer(rtc::IceCredentials::generate(), rtc::Certificate::generate()->getFingerprint(),
                                std::array{asio::ip::udp::endpoint{asio::ip::make_address("127.0.0.1"), 41234}})
            .generate(1);
    std::optional<std::expected<SignalingClient::Answer, ESessionError>> result;
    asio::co_spawn(io_, SignalingClient::requestAnswer(io_, "127.0.0.1", port_, "endgate", offer, config_),
                   [&](const std::exception_ptr &, std::expected<SignalingClient::Answer, ESessionError> answer) {
                       result = std::move(answer);
                       io_.stop();
                   });
    io_.run();

    ASSERT_TRUE(result);
    ASSERT_TRUE(result->has_value());
    EXPECT_TRUE((*result)->sdp.starts_with("v=0"));
    EXPECT_EQ((*result)->sdp.find("a=identity"), std::string::npos);
    ASSERT_TRUE((*result)->identity);
    EXPECT_TRUE((*result)->identity->verified);
    EXPECT_EQ((*result)->identity->idp_domain, "self");
    EXPECT_FALSE((*result)->identity->cpk_digest.empty());
}

TEST_F(SignalingClientTest, RequestAnswerRaisesTheServersNumericError)
{
    handler_.rejectWith(static_cast<std::uint32_t>(ESessionError::IdentityNotAllowed));
    listen();
    const auto offer =
        rtc::Description::offer(rtc::IceCredentials::generate(), rtc::Certificate::generate()->getFingerprint(),
                                std::array{asio::ip::udp::endpoint{asio::ip::make_address("127.0.0.1"), 41234}})
            .generate(1);
    std::optional<std::expected<SignalingClient::Answer, ESessionError>> result;
    asio::co_spawn(io_, SignalingClient::requestAnswer(io_, "127.0.0.1", port_, "endgate", offer, config_),
                   [&](const std::exception_ptr &, std::expected<SignalingClient::Answer, ESessionError> answer) {
                       result = std::move(answer);
                       io_.stop();
                   });
    io_.run();

    ASSERT_TRUE(result);
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error(), ESessionError::IdentityNotAllowed);
}

}  // namespace
}  // namespace endgate::nethernet
