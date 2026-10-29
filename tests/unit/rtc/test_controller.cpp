#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <asio/ip/udp.hpp>
#include <gtest/gtest.h>

#include "endgate/network/rtc/ice.h"
#include "endgate/network/rtc/stun.h"

namespace endgate::rtc {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct Datagram {
    asio::ip::udp::endpoint source;
    std::vector<std::byte> bytes;
};

class ControllerListener : public IceController::Listener {
public:
    explicit ControllerListener(asio::ip::udp::endpoint self) : self_(std::move(self)) {}

    void sendIceDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) override
    {
        outgoing.push_back(
            {remote, {.source = self_, .bytes = std::vector<std::byte>(datagram.begin(), datagram.end())}});
    }
    void onSelectedPairChanged(const std::optional<asio::ip::udp::endpoint> &remote) override
    {
        selections.push_back(remote);
    }
    void onIceStateChanged(IceController::State state) override { states.push_back(state); }

    std::deque<std::pair<asio::ip::udp::endpoint, Datagram>> outgoing;
    std::vector<std::optional<asio::ip::udp::endpoint>> selections;
    std::vector<IceController::State> states;

private:
    asio::ip::udp::endpoint self_;
};

class LiteListener : public IceTransport::Listener {
public:
    explicit LiteListener(asio::ip::udp::endpoint self) : self_(std::move(self)) {}

    void sendIceDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) override
    {
        outgoing.push_back(
            {remote, {.source = self_, .bytes = std::vector<std::byte>(datagram.begin(), datagram.end())}});
    }
    void onCandidatePairAdded(const asio::ip::udp::endpoint &) override {}
    void onCandidatePairRemoved(const asio::ip::udp::endpoint &) override {}
    void onSelectedPairChanged(const std::optional<asio::ip::udp::endpoint> &) override {}
    void onIceStateChanged(IceTransport::State) override {}

    std::deque<std::pair<asio::ip::udp::endpoint, Datagram>> outgoing;

private:
    asio::ip::udp::endpoint self_;
};

class IceControllerTest : public testing::Test {
protected:
    IceControllerTest()
        : controller_(controller_listener_, controller_creds_, lite_creds_.ufrag, lite_creds_.pwd,
                      std::vector{lite_address_}, 0x0123456789ABCDEFULL, {}, now_),
          lite_(lite_listener_, lite_creds_, controller_creds_.ufrag, {}, now_)
    {
    }

    void deliver()
    {
        while (!controller_listener_.outgoing.empty() || !lite_listener_.outgoing.empty()) {
            while (!controller_listener_.outgoing.empty()) {
                auto [destination, datagram] = std::move(controller_listener_.outgoing.front());
                controller_listener_.outgoing.pop_front();
                if (destination == lite_address_) {
                    lite_.handleDatagram(now_, datagram.source, datagram.bytes);
                }
            }
            while (!lite_listener_.outgoing.empty()) {
                auto [destination, datagram] = std::move(lite_listener_.outgoing.front());
                lite_listener_.outgoing.pop_front();
                if (destination == controller_address_) {
                    controller_.handleDatagram(now_, datagram.source, datagram.bytes);
                }
            }
        }
    }

    bool pump(IceController::State until)
    {
        for (int round = 0; round < 10000; ++round) {
            deliver();
            if (controller_.getState() == until) {
                return true;
            }
            const auto controller_next = controller_.nextTimeout();
            const auto lite_next = lite_.nextTimeout();
            std::optional<Clock::time_point> next = controller_next;
            if (lite_next && (!next || *lite_next < *next)) {
                next = lite_next;
            }
            if (!next) {
                return controller_.getState() == until;
            }
            now_ = std::max(now_, *next);
            if (controller_next && *controller_next <= now_) {
                controller_.handleTimeout(now_);
            }
            if (lite_next && *lite_next <= now_) {
                lite_.handleTimeout(now_);
            }
        }
        return false;
    }

    Clock::time_point now_{Clock::now()};
    IceCredentials controller_creds_{IceCredentials::generate()};
    IceCredentials lite_creds_{IceCredentials::generate()};
    asio::ip::udp::endpoint controller_address_{asio::ip::make_address("203.0.113.7"), 41234};
    asio::ip::udp::endpoint lite_address_{asio::ip::make_address("198.51.100.8"), 19132};
    ControllerListener controller_listener_{controller_address_};
    LiteListener lite_listener_{lite_address_};
    IceController controller_;
    IceTransport lite_;
};

TEST_F(IceControllerTest, ConnectsAndNominatesTheLiteAgent)
{
    ASSERT_TRUE(pump(IceController::State::Connected));
    EXPECT_EQ(controller_.getState(), IceController::State::Connected);
    EXPECT_EQ(controller_.getSelectedPair(), lite_address_);
    EXPECT_EQ(lite_.getSelectedPair(), controller_address_);
    EXPECT_EQ(lite_.getState(), IceTransport::State::Connected);
    EXPECT_GE(controller_.getStatistics().responses_accepted, 2U);
    ASSERT_FALSE(controller_listener_.selections.empty());
    EXPECT_EQ(controller_listener_.selections.back(), lite_address_);
}

TEST_F(IceControllerTest, ForwardsApplicationDataOnlyOnceConnected)
{
    const std::vector<std::byte> payload{std::byte{20}, std::byte{1}, std::byte{2}};
    EXPECT_FALSE(controller_.send(payload));
    ASSERT_TRUE(pump(IceController::State::Connected));
    controller_listener_.outgoing.clear();
    EXPECT_TRUE(controller_.send(payload));
    ASSERT_EQ(controller_listener_.outgoing.size(), 1U);
    EXPECT_EQ(controller_listener_.outgoing.front().first, lite_address_);
    EXPECT_EQ(controller_listener_.outgoing.front().second.bytes, payload);
}

TEST_F(IceControllerTest, AnswersTheChecksThePeerSends)
{
    ASSERT_TRUE(pump(IceController::State::Connected));
    StunMessage peer_check;
    peer_check.transaction_id[0] = std::byte{0x55};
    peer_check.username = controller_creds_.ufrag + ":" + lite_creds_.ufrag;
    peer_check.priority = 1234567U;
    peer_check.ice_controlled = 0x5555555555555555ULL;
    controller_listener_.outgoing.clear();
    controller_.handleDatagram(now_, lite_address_, peer_check.encode(controller_creds_.pwd));
    ASSERT_EQ(controller_listener_.outgoing.size(), 1U);
    const auto response = StunMessage::decode(controller_listener_.outgoing.front().second.bytes);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->message_class, StunClass::SuccessResponse);
    EXPECT_TRUE(response->verifyIntegrity(controller_listener_.outgoing.front().second.bytes, controller_creds_.pwd));
    EXPECT_EQ(response->xor_mapped_address, lite_address_);
}

TEST_F(IceControllerTest, FailsWhenNoRemoteAnswers)
{
    IceController::Config quick{.negotiation_timeout = 500ms};
    ControllerListener listener{controller_address_};
    IceController alone(listener, controller_creds_, lite_creds_.ufrag, lite_creds_.pwd, std::vector{lite_address_}, 1,
                        quick, now_);
    for (auto deadline = alone.nextTimeout(); deadline && alone.getState() != IceController::State::Failed;
         deadline = alone.nextTimeout()) {
        now_ = *deadline;
        listener.outgoing.clear();
        alone.handleTimeout(now_);
    }
    EXPECT_EQ(alone.getState(), IceController::State::Failed);
    EXPECT_FALSE(alone.nextTimeout());
}

TEST_F(IceControllerTest, YieldsTheControllingRoleOnA487)
{
    now_ = now_ + 1ms;
    controller_.handleTimeout(now_);
    ASSERT_FALSE(controller_listener_.outgoing.empty());
    const auto request = StunMessage::decode(controller_listener_.outgoing.front().second.bytes);
    ASSERT_TRUE(request);
    ASSERT_TRUE(request->ice_controlling);

    StunMessage conflict;
    conflict.message_class = StunClass::ErrorResponse;
    conflict.transaction_id = request->transaction_id;
    conflict.error_code = StunErrorCode{.code = 487, .reason = "Role Conflict"};
    controller_listener_.outgoing.clear();
    controller_.handleDatagram(now_, lite_address_, conflict.encode(lite_creds_.pwd));
    EXPECT_EQ(controller_.getRole(), IceController::Role::Controlled);

    now_ = now_ + 1ms;
    controller_.handleTimeout(now_);
    ASSERT_FALSE(controller_listener_.outgoing.empty());
    const auto retry = StunMessage::decode(controller_listener_.outgoing.front().second.bytes);
    ASSERT_TRUE(retry);
    EXPECT_FALSE(retry->ice_controlling);
    EXPECT_TRUE(retry->ice_controlled);
}

}  // namespace
}  // namespace endgate::rtc
