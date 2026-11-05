#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "endgate/config.h"

namespace endgate {
namespace {

class ProxyConfigTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        directory_ = std::filesystem::path{::testing::TempDir()} /
                     ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override { std::filesystem::remove_all(directory_); }

    [[nodiscard]] ProxyConfig readAdvanced(const std::string &advanced) const
    {
        const auto path = directory_ / "endgate.toml";
        std::ofstream{path} << "[advanced]\n" << advanced;
        auto config = ProxyConfig::read(path);
        EXPECT_TRUE(config.has_value());
        return *config;
    }

    [[nodiscard]] ProxyConfig readDocument(const std::string &document) const
    {
        const auto path = directory_ / "endgate.toml";
        std::ofstream{path} << document;
        auto config = ProxyConfig::read(path);
        EXPECT_TRUE(config.has_value());
        return *config;
    }

    std::filesystem::path directory_;
};

}  // namespace

TEST_F(ProxyConfigTest, TimingSettingsDefaultToVelocitys)
{
    auto config = ProxyConfig::read(directory_ / "endgate.toml");
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->getLoginRatelimit(), 3000);
    EXPECT_EQ(config->getConnectTimeout(), 5000);
    EXPECT_EQ(config->getReadTimeout(), 30000);
}

TEST_F(ProxyConfigTest, ReadsTimingSettings)
{
    const auto config = readAdvanced("login-ratelimit = 0\nconnection-timeout = 1234\nread-timeout = 4321\n");
    EXPECT_EQ(config.getLoginRatelimit(), 0);
    EXPECT_EQ(config.getConnectTimeout(), 1234);
    EXPECT_EQ(config.getReadTimeout(), 4321);
}

TEST_F(ProxyConfigTest, AcceptsLoginRatelimitOfZero)
{
    EXPECT_TRUE(readAdvanced("login-ratelimit = 0\n").validate());
}

TEST_F(ProxyConfigTest, RejectsNegativeLoginRatelimit)
{
    EXPECT_FALSE(readAdvanced("login-ratelimit = -1\n").validate());
}

TEST_F(ProxyConfigTest, ForcedHostsDefaultToTheExampleHost)
{
    auto config = ProxyConfig::read(directory_ / "endgate.toml");
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->getForcedHosts().at("lobby.example.com"), std::vector<std::string>{"lobby"});
    EXPECT_TRUE(config->validate());
}

TEST_F(ProxyConfigTest, ForcedHostsAreLowercasedAndTakeAStringOrAList)
{
    const auto config = readDocument(
        "[servers]\nlobby = \"127.0.0.1:1\"\nfactions = \"127.0.0.1:2\"\ntry = [\"lobby\"]\n"
        "[forced-hosts]\n\"Factions.Example.COM\" = \"factions\"\n\"play.example.com\" = [\"factions\", \"lobby\"]\n");
    EXPECT_EQ(config.getForcedHosts().size(), 2U);
    EXPECT_EQ(config.getForcedHosts().at("factions.example.com"), std::vector<std::string>{"factions"});
    EXPECT_EQ(config.getForcedHosts().at("play.example.com"), (std::vector<std::string>{"factions", "lobby"}));
    EXPECT_TRUE(config.validate());
}

TEST_F(ProxyConfigTest, AnEmptyForcedHostsSectionRemovesTheDefaults)
{
    const auto config = readDocument("[forced-hosts]\n");
    EXPECT_TRUE(config.getForcedHosts().empty());
}

TEST_F(ProxyConfigTest, RejectsAForcedHostNamingAnUnknownServer)
{
    EXPECT_FALSE(readDocument("[forced-hosts]\n\"play.example.com\" = [\"nowhere\"]\n").validate());
}

TEST_F(ProxyConfigTest, RejectsAForcedHostWithoutServers)
{
    EXPECT_FALSE(readDocument("[forced-hosts]\n\"play.example.com\" = []\n").validate());
}

TEST_F(ProxyConfigTest, PlayerInfoForwardingDefaultsToNone)
{
    auto config = ProxyConfig::read(directory_ / "endgate.toml");
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->getPlayerInfoForwardingMode(), PlayerInfoForwarding::None);
}

TEST_F(ProxyConfigTest, PlayerInfoForwardingModeIsCaseInsensitive)
{
    EXPECT_EQ(readDocument("player-info-forwarding-mode = \"MODERN\"\n").getPlayerInfoForwardingMode(),
              PlayerInfoForwarding::Modern);
}

TEST_F(ProxyConfigTest, RejectsAnUnknownPlayerInfoForwardingMode)
{
    std::ofstream{directory_ / "endgate.toml"} << "player-info-forwarding-mode = \"legacy\"\n";
    EXPECT_FALSE(ProxyConfig::read(directory_ / "endgate.toml").has_value());
}

TEST_F(ProxyConfigTest, AcceptsModernForwardingInOnlineMode)
{
    EXPECT_TRUE(readDocument("player-info-forwarding-mode = \"modern\"\n").validate());
}

TEST_F(ProxyConfigTest, RejectsModernForwardingInOfflineMode)
{
    EXPECT_FALSE(readDocument("online-mode = false\nplayer-info-forwarding-mode = \"modern\"\n").validate());
}

TEST_F(ProxyConfigTest, AcceptsNoForwardingInOfflineMode)
{
    EXPECT_TRUE(readDocument("online-mode = false\n").validate());
}

}  // namespace endgate
