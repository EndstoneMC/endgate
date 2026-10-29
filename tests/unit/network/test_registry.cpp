#include <memory>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "support.h"

namespace endgate {
namespace {

class RegistryTest : public testing::Test {
protected:
    [[nodiscard]] static Registry::Claim claim(Registry &registry, Node &node, const Join &join, std::string_view key)
    {
        const auto datagram = makeCheck(join, key);
        const auto decoded = rtc::StunMessage::decode(datagram.slice());
        return registry.claim(join.local.ufrag, node.endpoint(), *decoded, datagram.slice());
    }

    ProxyServer server_;
};

TEST_F(RegistryTest, GuidsAreHeldOnceUntilRemoved)
{
    Registry registry;
    EXPECT_TRUE(registry.addGuid(7));
    EXPECT_FALSE(registry.addGuid(7));
    registry.removeGuid(7);
    EXPECT_TRUE(registry.addGuid(7));
}

TEST_F(RegistryTest, HalfOpenStopsAtTheLimitAndFreesOnRemove)
{
    Registry registry(2, 8);
    EXPECT_TRUE(registry.addHalfOpen());
    EXPECT_TRUE(registry.addHalfOpen());
    EXPECT_FALSE(registry.addHalfOpen());
    registry.removeHalfOpen();
    EXPECT_TRUE(registry.addHalfOpen());
}

TEST_F(RegistryTest, RemovingMoreHalfOpenThanAddedDoesNotUnderflow)
{
    Registry registry(1, 8);
    registry.removeHalfOpen();
    EXPECT_TRUE(registry.addHalfOpen());
    EXPECT_FALSE(registry.addHalfOpen());
}

TEST_F(RegistryTest, PendingJoinsStopAtTheCap)
{
    Registry registry(8, 2);
    const auto first = makeJoin();
    const auto second = makeJoin();
    const auto third = makeJoin();
    EXPECT_EQ(registry.addPending(makePending(first.local, first.remote)), Registry::AddPending::Added);
    EXPECT_EQ(registry.addPending(makePending(second.local, second.remote)), Registry::AddPending::Added);
    EXPECT_EQ(registry.addPending(makePending(third.local, third.remote)), Registry::AddPending::Full);
    registry.removePending(first.local.ufrag);
    EXPECT_EQ(registry.addPending(makePending(third.local, third.remote)), Registry::AddPending::Added);
}

TEST_F(RegistryTest, APendingUfragCollides)
{
    Registry registry;
    const auto join = makeJoin();
    EXPECT_EQ(registry.addPending(makePending(join.local, join.remote)), Registry::AddPending::Added);
    EXPECT_EQ(registry.addPending(makePending(join.local, join.remote)), Registry::AddPending::Collision);
}

TEST_F(RegistryTest, AClaimedUfragCollidesUntilReleased)
{
    Registry registry;
    Node node(server_, registry);
    const auto join = makeJoin();
    ASSERT_EQ(registry.addPending(makePending(join.local, join.remote)), Registry::AddPending::Added);
    ASSERT_TRUE(std::holds_alternative<PendingJoin>(claim(registry, node, join, join.local.pwd)));
    EXPECT_EQ(registry.addPending(makePending(join.local, join.remote)), Registry::AddPending::Collision);
    registry.release(join.local.ufrag);
    EXPECT_EQ(registry.addPending(makePending(join.local, join.remote)), Registry::AddPending::Added);
}

TEST_F(RegistryTest, TheFirstClaimGetsTheJoinAndLaterOnesGetTheOwner)
{
    Registry registry;
    Node first(server_, registry);
    Node second(server_, registry);
    const auto join = makeJoin();
    ASSERT_EQ(registry.addPending(makePending(join.local, join.remote)), Registry::AddPending::Added);

    const auto claimed = claim(registry, first, join, join.local.pwd);
    ASSERT_TRUE(std::holds_alternative<PendingJoin>(claimed));
    EXPECT_EQ(std::get<PendingJoin>(claimed).network_id, "test");

    const auto later = claim(registry, second, join, join.local.pwd);
    ASSERT_TRUE(std::holds_alternative<Endpoint *>(later));
    EXPECT_EQ(std::get<Endpoint *>(later), &first.endpoint());
}

TEST_F(RegistryTest, AClaimFromTheWrongKeyUsernameOrClassIsRefused)
{
    Registry registry;
    Node node(server_, registry);
    const auto join = makeJoin();
    ASSERT_EQ(registry.addPending(makePending(join.local, join.remote)), Registry::AddPending::Added);

    EXPECT_TRUE(std::holds_alternative<std::monostate>(claim(registry, node, join, "not-the-password")));

    auto wrong_user = join;
    wrong_user.username = join.local.ufrag + ":" + rtc::IceCredentials::generate().ufrag;
    EXPECT_TRUE(std::holds_alternative<std::monostate>(claim(registry, node, wrong_user, join.local.pwd)));

    ASSERT_TRUE(std::holds_alternative<PendingJoin>(claim(registry, node, join, join.local.pwd)));
    EXPECT_TRUE(std::holds_alternative<std::monostate>(claim(registry, node, join, "not-the-password")));
}

TEST_F(RegistryTest, AnUnknownUfragClaimsNothing)
{
    Registry registry;
    Node node(server_, registry);
    const auto join = makeJoin();
    EXPECT_TRUE(std::holds_alternative<std::monostate>(claim(registry, node, join, join.local.pwd)));
}

TEST_F(RegistryTest, SessionIdsAreUnique)
{
    Registry registry;
    EXPECT_NE(registry.nextSessionId(), registry.nextSessionId());
}

}  // namespace
}  // namespace endgate
