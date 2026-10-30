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

}  // namespace endgate
