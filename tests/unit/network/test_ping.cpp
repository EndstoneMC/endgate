#include <string>

#include <gtest/gtest.h>

#include "endgate/network/ping.h"

namespace endgate {
namespace {

TEST(ServerPing, AnAnnouncementReadsBackAsThePingThatWroteIt)
{
    const ServerPing ping{ServerPing::Version{2225, "1.26.60"}, ServerPing::Players{3, 10}, "Endgate Test",
                          "Bedrock level", GameType::Creative};

    const auto parsed = ServerPing::parseMotd(ping.toMotd(0x1122334455667788ULL, 19132));

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->getVersion().getProtocol(), 2225);
    EXPECT_EQ(parsed->getVersion().getName(), "1.26.60");
    EXPECT_EQ(parsed->getPlayers().getOnline(), 3);
    EXPECT_EQ(parsed->getPlayers().getMax(), 10);
    EXPECT_EQ(parsed->getDescription(), "Endgate Test");
    EXPECT_EQ(parsed->getLevelName(), "Bedrock level");
    EXPECT_EQ(parsed->getGameType(), GameType::Creative);
}

TEST(ServerPing, ABedrockServersAnnouncementGivesItsProtocol)
{
    const auto parsed = ServerPing::parseMotd(
        "MCPE;Dedicated Server;2225;1.26.60;0;10;13253860892328930865;Bedrock level;Survival;1;19132;19133;0;");

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->getVersion().getProtocol(), 2225);
    EXPECT_EQ(parsed->getLevelName(), "Bedrock level");
    EXPECT_EQ(parsed->getGameType(), GameType::Survival);
}

TEST(ServerPing, AnAnnouncementThatStopsAfterThePlayerCountsStillGivesItsProtocol)
{
    const auto parsed = ServerPing::parseMotd("MCPE;Dedicated Server;2193;1.26.50;0;10");

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->getVersion().getProtocol(), 2193);
    EXPECT_EQ(parsed->getPlayers().getMax(), 10);
    EXPECT_EQ(parsed->getLevelName(), "");
    EXPECT_EQ(parsed->getGameType(), GameType::Survival);
}

TEST(ServerPing, SomethingThatIsNotABedrockAnnouncementIsRefused)
{
    EXPECT_FALSE(ServerPing::parseMotd("").has_value());
    EXPECT_FALSE(ServerPing::parseMotd("MCPE").has_value());
    EXPECT_FALSE(ServerPing::parseMotd("MCPE;Dedicated Server").has_value());
    EXPECT_FALSE(ServerPing::parseMotd("MCPE;Dedicated Server;;1.26.60;0;10").has_value());
    EXPECT_FALSE(ServerPing::parseMotd("MCPE;Dedicated Server;2225x;1.26.60;0;10").has_value());
    EXPECT_FALSE(ServerPing::parseMotd("MCJE;A Java Server;771;1.21.8;0;10").has_value());
}

TEST(ServerPing, AGameTypeTheProxyDoesNotKnowReadsAsSurvival)
{
    const auto parsed = ServerPing::parseMotd("MCPE;Dedicated Server;2225;1.26.60;0;10;1;Bedrock level;Hardcore;1;");

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->getGameType(), GameType::Survival);
}

}  // namespace
}  // namespace endgate
