#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "support.h"

namespace endgate {
namespace {

using namespace std::chrono_literals;

class EndpointTest : public testing::Test {
protected:
    [[nodiscard]] Join join()
    {
        auto created = makeJoin();
        EXPECT_EQ(registry_.addPending(makePending(created.local, created.remote)), Registry::AddPending::Added);
        return created;
    }

    [[nodiscard]] static bool succeeded(
        const std::optional<std::pair<rtc::StunMessage, asio::ip::udp::endpoint>> &reply)
    {
        return reply && reply->first.message_class == rtc::StunClass::SuccessResponse;
    }

    template <typename Predicate>
    [[nodiscard]] static bool eventually(Predicate &&predicate)
    {
        for (int i = 0; i < 100; ++i) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

    ProxyServer server_;
    Registry registry_;
};

TEST_F(EndpointTest, TheFirstVerifiedCheckCreatesTheSessionOnTheLoopThatReceivedIt)
{
    Node node(server_, registry_);
    const auto created = join();
    Client client;
    client.send(node.address(), makeCheck(created, created.local.pwd).slice());

    const auto reply = client.receive();
    ASSERT_TRUE(succeeded(reply));
    EXPECT_EQ(reply->second, node.address());
    EXPECT_EQ(node.connections(), 1U);
}

TEST_F(EndpointTest, AnUnverifiedCheckIsNeverClaimed)
{
    Node node(server_, registry_);
    const auto created = join();
    Client client;
    client.send(node.address(), makeCheck(created, "not-the-password").slice());

    EXPECT_FALSE(client.receive(200ms));
    EXPECT_EQ(node.connections(), 0U);

    client.send(node.address(), makeCheck(created, created.local.pwd).slice());
    EXPECT_TRUE(succeeded(client.receive()));
    EXPECT_EQ(node.connections(), 1U);
}

TEST_F(EndpointTest, ACheckWithANewUfragIsClaimedFromAnAddressThatStillHasARoute)
{
    Node node(server_, registry_);
    const auto first = join();
    Client client;
    client.send(node.address(), makeCheck(first, first.local.pwd).slice());
    ASSERT_TRUE(succeeded(client.receive()));

    const auto second = join();
    client.send(node.address(), makeCheck(second, second.local.pwd).slice());
    EXPECT_TRUE(succeeded(client.receive()));
    EXPECT_EQ(node.connections(), 2U);
}

TEST_F(EndpointTest, ACheckFromAnotherAddressIsHandedToTheOwnerAndCached)
{
    Node owner(server_, registry_);
    Node other(server_, registry_);
    const auto created = join();
    Client first;
    Client second;
    first.send(owner.address(), makeCheck(created, created.local.pwd).slice());
    ASSERT_TRUE(succeeded(first.receive()));

    second.send(other.address(), makeCheck(created, created.local.pwd).slice());
    const auto reply = second.receive();
    ASSERT_TRUE(succeeded(reply));
    EXPECT_EQ(reply->second, owner.address());
    EXPECT_EQ(other.connections(), 0U);
    EXPECT_EQ(other.forwards(), 1U);
}

TEST_F(EndpointTest, AnUnverifiedCheckIsNeverForwardedOrCached)
{
    Node owner(server_, registry_);
    Node other(server_, registry_);
    const auto created = join();
    Client first;
    Client second;
    first.send(owner.address(), makeCheck(created, created.local.pwd).slice());
    ASSERT_TRUE(succeeded(first.receive()));

    second.send(other.address(), makeCheck(created, "not-the-password").slice());
    EXPECT_FALSE(second.receive(200ms));
    EXPECT_EQ(other.forwards(), 0U);
}

TEST_F(EndpointTest, ATrafficEntryForAnEndedSessionIsInvalidated)
{
    Node owner(server_, registry_);
    Node other(server_, registry_);
    const auto created = join();
    Client first;
    Client second;
    first.send(owner.address(), makeCheck(created, created.local.pwd).slice());
    ASSERT_TRUE(succeeded(first.receive()));
    second.send(other.address(), makeCheck(created, created.local.pwd).slice());
    ASSERT_TRUE(succeeded(second.receive()));
    ASSERT_EQ(other.forwards(), 1U);

    owner.close();
    const std::array<std::uint8_t, 4> dtls{0x16, 0xfe, 0xfd, 0x00};
    second.send(other.address(), Unpooled::copiedBuffer(std::as_bytes(std::span{dtls})).slice());
    EXPECT_TRUE(eventually([&] { return other.forwards() == 0; }));
}

TEST_F(EndpointTest, ARejoinFromTheSameAddressSucceedsWithAStaleEntry)
{
    Node owner(server_, registry_);
    Node other(server_, registry_);
    const auto created = join();
    Client first;
    Client second;
    first.send(owner.address(), makeCheck(created, created.local.pwd).slice());
    ASSERT_TRUE(succeeded(first.receive()));
    second.send(other.address(), makeCheck(created, created.local.pwd).slice());
    ASSERT_TRUE(succeeded(second.receive()));

    owner.close();
    const auto rejoined = join();
    second.send(other.address(), makeCheck(rejoined, rejoined.local.pwd).slice());
    const auto reply = second.receive();
    ASSERT_TRUE(succeeded(reply));
    EXPECT_EQ(reply->second, other.address());
    EXPECT_EQ(other.connections(), 1U);
    EXPECT_EQ(other.forwards(), 0U);
}

TEST_F(EndpointTest, TwoLoopsRacingForTheFirstCheckCreateOneSession)
{
    Node first_node(server_, registry_);
    Node second_node(server_, registry_);
    for (int round = 0; round < 50; ++round) {
        const auto before = first_node.connections() + second_node.connections();
        const auto created = join();
        Client first;
        Client second;
        const auto first_check = makeCheck(created, created.local.pwd);
        const auto second_check = makeCheck(created, created.local.pwd);
        std::atomic<bool> go{false};
        std::thread sender([&] {
            while (!go) {
                std::this_thread::yield();
            }
            second.send(second_node.address(), second_check.slice());
        });
        go = true;
        first.send(first_node.address(), first_check.slice());
        sender.join();

        EXPECT_TRUE(succeeded(first.receive())) << round;
        EXPECT_TRUE(succeeded(second.receive())) << round;
        EXPECT_EQ(first_node.connections() + second_node.connections(), before + 1) << round;
    }
}

}  // namespace
}  // namespace endgate
