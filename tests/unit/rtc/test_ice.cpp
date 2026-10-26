#include <libdeflate.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "endgate/network/rtc/ice.h"
#include "endgate/network/rtc/stun.h"

namespace endgate::rtc {
namespace {

using namespace std::chrono_literals;

class RecordingListener : public IceTransport::Listener {
public:
    void sendIceDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) override
    {
        sent.emplace_back(remote, std::vector<std::byte>(datagram.begin(), datagram.end()));
    }
    void onCandidatePairAdded(const asio::ip::udp::endpoint &remote) override { added.push_back(remote); }
    void onCandidatePairRemoved(const asio::ip::udp::endpoint &remote) override { removed.push_back(remote); }
    void onSelectedPairChanged(const std::optional<asio::ip::udp::endpoint> &remote) override
    {
        selections.push_back(remote);
    }
    void onIceStateChanged(IceTransport::State state) override { states.push_back(state); }

    std::vector<std::pair<asio::ip::udp::endpoint, std::vector<std::byte>>> sent;
    std::vector<asio::ip::udp::endpoint> added;
    std::vector<asio::ip::udp::endpoint> removed;
    std::vector<std::optional<asio::ip::udp::endpoint>> selections;
    std::vector<IceTransport::State> states;
};

class IceTransportTest : public testing::Test {
protected:
    [[nodiscard]] StunMessage check(bool use_candidate) const
    {
        StunMessage request;
        request.transaction_id[0] = static_cast<std::byte>(next_transaction_++);
        request.username = lite_.ufrag + ":" + full_.ufrag;
        request.priority = 1845501695;
        request.ice_controlling = 0x1122334455667788ULL;
        request.use_candidate = use_candidate;
        return request;
    }

    void deliver(const asio::ip::udp::endpoint &from, const StunMessage &request, std::string_view key)
    {
        transport_.handleDatagram(now_, from, request.encode(key));
    }

    [[nodiscard]] StunMessage lastResponse() const
    {
        const auto decoded = StunMessage::decode(listener_.sent.back().second);
        EXPECT_TRUE(decoded);
        EXPECT_TRUE(decoded->fingerprint);
        EXPECT_TRUE(decoded->verifyIntegrity(listener_.sent.back().second, lite_.pwd));
        return *decoded;
    }

    std::chrono::steady_clock::time_point now_{std::chrono::steady_clock::time_point{} + 1h};
    IceCredentials lite_{IceCredentials::generate()};
    IceCredentials full_{IceCredentials::generate()};
    RecordingListener listener_;
    IceTransport transport_{listener_, lite_, full_.ufrag, {}, now_};
    asio::ip::udp::endpoint host_{asio::ip::make_address("198.51.100.7"), 50000};
    asio::ip::udp::endpoint other_{asio::ip::make_address("2001:db8::7"), 50001};
    mutable std::uint8_t next_transaction_{1};
};

TEST(IceCredentialsTest, GeneratesIceCharsOfTheRequiredLengths)
{
    std::set<std::string> ufrags;
    for (int i = 0; i < 64; ++i) {
        const auto credentials = IceCredentials::generate();
        EXPECT_GE(credentials.ufrag.size(), 4U);
        EXPECT_LE(credentials.ufrag.size(), 32U);
        EXPECT_GE(credentials.pwd.size(), 22U);
        for (const char c : credentials.ufrag + credentials.pwd) {
            EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/');
        }
        ufrags.insert(credentials.ufrag);
    }
    EXPECT_EQ(ufrags.size(), 64U);
}

TEST_F(IceTransportTest, AnswersAValidCheckWithTheMappedAddress)
{
    const auto request = check(false);
    deliver(host_, request, lite_.pwd);

    ASSERT_EQ(listener_.sent.size(), 1U);
    EXPECT_EQ(listener_.sent.back().first, host_);
    const auto response = lastResponse();
    EXPECT_EQ(response.message_class, StunClass::SuccessResponse);
    EXPECT_EQ(response.transaction_id, request.transaction_id);
    EXPECT_EQ(response.xor_mapped_address, host_);
    EXPECT_EQ(listener_.added, std::vector{host_});
    EXPECT_EQ(transport_.getState(), IceTransport::State::Checking);
    EXPECT_FALSE(transport_.getSelectedPair());
}

TEST_F(IceTransportTest, ReportsAnIpv4MappedSourceAsIpv4)
{
    const asio::ip::udp::endpoint mapped{asio::ip::make_address("::ffff:198.51.100.7"), 50000};
    deliver(mapped, check(false), lite_.pwd);
    EXPECT_EQ(listener_.sent.back().first, mapped);
    EXPECT_EQ(lastResponse().xor_mapped_address, host_);
}

TEST_F(IceTransportTest, DropsChecksThatFailAuthentication)
{
    deliver(host_, check(false), full_.pwd);

    auto wrong_local = check(false);
    wrong_local.username = "nope:" + full_.ufrag;
    deliver(host_, wrong_local, lite_.pwd);

    auto wrong_remote = check(false);
    wrong_remote.username = lite_.ufrag + ":nope";
    deliver(host_, wrong_remote, lite_.pwd);

    auto no_username = check(false);
    no_username.username.reset();
    deliver(host_, no_username, lite_.pwd);

    transport_.handleDatagram(now_, host_, check(false).encode(std::nullopt));

    auto without_fingerprint = check(false).encode(lite_.pwd);
    without_fingerprint.resize(without_fingerprint.size() - 8);
    without_fingerprint[3] = static_cast<std::byte>(without_fingerprint.size() - 20);
    transport_.handleDatagram(now_, host_, without_fingerprint);

    EXPECT_TRUE(listener_.sent.empty());
    EXPECT_TRUE(listener_.added.empty());
    EXPECT_EQ(transport_.getStatistics().datagrams_dropped, 6U);
}

TEST_F(IceTransportTest, DropsWhatIsNotABindingRequest)
{
    const std::array<std::byte, 4> garbage{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    transport_.handleDatagram(now_, host_, garbage);

    auto response = check(false);
    response.message_class = StunClass::SuccessResponse;
    deliver(host_, response, lite_.pwd);

    auto other_method = check(false);
    other_method.method = static_cast<StunMethod>(0x200);
    deliver(host_, other_method, lite_.pwd);

    EXPECT_TRUE(listener_.sent.empty());
    EXPECT_EQ(transport_.getStatistics().datagrams_dropped, 3U);
}

TEST_F(IceTransportTest, AcceptsBindingIndicationsSilently)
{
    StunMessage indication;
    indication.message_class = StunClass::Indication;
    transport_.handleDatagram(now_, host_, indication.encode(std::nullopt));
    EXPECT_TRUE(listener_.sent.empty());
    EXPECT_TRUE(listener_.added.empty());
    EXPECT_EQ(transport_.getStatistics().datagrams_dropped, 0U);
}

TEST_F(IceTransportTest, SelectsTheNominatedPair)
{
    EXPECT_FALSE(transport_.send(std::array<std::byte, 1>{}));
    deliver(host_, check(false), lite_.pwd);
    deliver(host_, check(true), lite_.pwd);

    EXPECT_EQ(transport_.getSelectedPair(), host_);
    EXPECT_EQ(transport_.getState(), IceTransport::State::Connected);
    EXPECT_EQ(listener_.selections, (std::vector<std::optional<asio::ip::udp::endpoint>>{host_}));
    EXPECT_EQ(listener_.states, std::vector{IceTransport::State::Connected});

    const std::array<std::byte, 3> payload{std::byte{22}, std::byte{254}, std::byte{253}};
    EXPECT_TRUE(transport_.send(payload));
    EXPECT_EQ(listener_.sent.back().first, host_);
    EXPECT_TRUE(std::ranges::equal(listener_.sent.back().second, payload));
}

TEST_F(IceTransportTest, FollowsARenomination)
{
    deliver(host_, check(true), lite_.pwd);
    deliver(other_, check(false), lite_.pwd);
    EXPECT_EQ(transport_.getSelectedPair(), host_);

    deliver(other_, check(true), lite_.pwd);
    EXPECT_EQ(transport_.getSelectedPair(), other_);
    EXPECT_EQ(listener_.selections, (std::vector<std::optional<asio::ip::udp::endpoint>>{host_, other_}));
    EXPECT_EQ(lastResponse().xor_mapped_address, other_);

    deliver(host_, check(false), lite_.pwd);
    EXPECT_EQ(transport_.getSelectedPair(), other_);
}

TEST_F(IceTransportTest, AnswersARoleConflictWithoutTakingThePair)
{
    auto controlled = check(true);
    controlled.ice_controlling.reset();
    controlled.ice_controlled = 0x99ULL;
    deliver(host_, controlled, lite_.pwd);

    const auto response = lastResponse();
    EXPECT_EQ(response.message_class, StunClass::ErrorResponse);
    ASSERT_TRUE(response.error_code);
    EXPECT_EQ(response.error_code->code, 487);
    EXPECT_EQ(response.transaction_id, controlled.transaction_id);
    EXPECT_TRUE(listener_.added.empty());
    EXPECT_FALSE(transport_.getSelectedPair());
}

TEST_F(IceTransportTest, RejectsACheckClaimingBothRoles)
{
    auto both = check(false);
    both.ice_controlled = 0x99ULL;
    deliver(host_, both, lite_.pwd);
    ASSERT_TRUE(lastResponse().error_code);
    EXPECT_EQ(lastResponse().error_code->code, 400);
    EXPECT_TRUE(listener_.added.empty());
}

TEST_F(IceTransportTest, AnswersAnUnknownRequiredAttributeWith420)
{
    auto datagram = check(false).encode(std::nullopt);
    datagram.resize(datagram.size() - 8);
    const std::array<std::byte, 8> unknown{std::byte{0x00}, std::byte{0x30}, std::byte{0x00}, std::byte{0x04},
                                           std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    datagram.insert(datagram.end(), unknown.begin(), unknown.end());

    datagram[3] = static_cast<std::byte>(datagram.size() - 20 + 24);
    std::array<unsigned char, 20> hmac{};
    unsigned int hmac_length = 0;
    HMAC(EVP_sha1(), lite_.pwd.data(), static_cast<int>(lite_.pwd.size()),
         reinterpret_cast<const unsigned char *>(datagram.data()), datagram.size(), hmac.data(), &hmac_length);
    const std::array<std::byte, 4> integrity_header{std::byte{0x00}, std::byte{0x08}, std::byte{0x00}, std::byte{0x14}};
    datagram.insert(datagram.end(), integrity_header.begin(), integrity_header.end());
    for (const auto byte : hmac) {
        datagram.push_back(static_cast<std::byte>(byte));
    }

    datagram[3] = static_cast<std::byte>(datagram.size() - 20 + 8);
    const auto crc = static_cast<std::uint32_t>(libdeflate_crc32(0, datagram.data(), datagram.size())) ^ 0x5354554EU;
    const std::array<std::byte, 8> fingerprint{std::byte{0x80},
                                               std::byte{0x28},
                                               std::byte{0x00},
                                               std::byte{0x04},
                                               static_cast<std::byte>(crc >> 24U),
                                               static_cast<std::byte>(crc >> 16U),
                                               static_cast<std::byte>(crc >> 8U),
                                               static_cast<std::byte>(crc)};
    datagram.insert(datagram.end(), fingerprint.begin(), fingerprint.end());

    transport_.handleDatagram(now_, host_, datagram);
    const auto response = lastResponse();
    ASSERT_TRUE(response.error_code);
    EXPECT_EQ(response.error_code->code, 420);
    EXPECT_EQ(response.unknown_attributes, std::vector<std::uint16_t>{0x0030});
    EXPECT_TRUE(listener_.added.empty());
}

TEST_F(IceTransportTest, FailsWhenNothingIsNominatedInTime)
{
    EXPECT_EQ(transport_.nextTimeout(), now_ + 10s);
    deliver(host_, check(false), lite_.pwd);
    transport_.handleTimeout(now_ + 9s);
    EXPECT_EQ(transport_.getState(), IceTransport::State::Checking);
    transport_.handleTimeout(now_ + 10s);
    EXPECT_EQ(transport_.getState(), IceTransport::State::Failed);
    EXPECT_EQ(listener_.states, std::vector{IceTransport::State::Failed});
    EXPECT_FALSE(transport_.nextTimeout());

    deliver(host_, check(true), lite_.pwd);
    EXPECT_EQ(listener_.sent.size(), 1U);
}

TEST_F(IceTransportTest, DropsAPairWhoseConsentExpires)
{
    deliver(host_, check(true), lite_.pwd);
    deliver(other_, check(false), lite_.pwd);
    now_ += 20s;
    deliver(other_, check(false), lite_.pwd);
    EXPECT_EQ(transport_.nextTimeout(), now_ + 10s);

    transport_.handleTimeout(now_ + 10s);
    EXPECT_EQ(listener_.removed, std::vector{host_});
    EXPECT_FALSE(transport_.getSelectedPair());
    EXPECT_EQ(transport_.getState(), IceTransport::State::Disconnected);
    EXPECT_FALSE(transport_.send(std::array<std::byte, 1>{}));

    now_ += 15s;
    deliver(other_, check(true), lite_.pwd);
    EXPECT_EQ(transport_.getSelectedPair(), other_);
    EXPECT_EQ(transport_.getState(), IceTransport::State::Connected);

    transport_.handleTimeout(now_ + 30s);
    EXPECT_EQ(transport_.getState(), IceTransport::State::Failed);
    EXPECT_EQ(listener_.removed, (std::vector{host_, other_}));
}

TEST_F(IceTransportTest, FallsBackToTheLatestNominatedPairThatIsStillAlive)
{
    deliver(other_, check(true), lite_.pwd);
    now_ += 5s;
    deliver(host_, check(true), lite_.pwd);
    now_ += 20s;
    deliver(other_, check(false), lite_.pwd);

    transport_.handleTimeout(now_ + 10s);
    EXPECT_EQ(listener_.removed, std::vector{host_});
    EXPECT_EQ(transport_.getSelectedPair(), other_);
    EXPECT_EQ(transport_.getState(), IceTransport::State::Connected);
}

TEST_F(IceTransportTest, BoundsThePairsItKeeps)
{
    deliver(host_, check(true), lite_.pwd);
    for (std::uint16_t port = 1; port <= 8; ++port) {
        now_ += 1s;
        deliver({asio::ip::make_address("192.0.2.1"), port}, check(false), lite_.pwd);
    }
    EXPECT_EQ(listener_.added.size(), 9U);
    EXPECT_EQ(listener_.removed, (std::vector<asio::ip::udp::endpoint>{{asio::ip::make_address("192.0.2.1"), 1}}));
    EXPECT_EQ(transport_.getSelectedPair(), host_);
}

}  // namespace
}  // namespace endgate::rtc
