#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "endgate/bstats/config.h"

namespace endgate {
namespace {

class MetricsConfigTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        directory_ = std::filesystem::path{::testing::TempDir()} /
                     ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::remove_all(directory_);
        file_ = directory_ / "plugins" / "bstats" / "config.toml";
    }

    void TearDown() override { std::filesystem::remove_all(directory_); }

    void write(const std::string &content) const
    {
        std::filesystem::create_directories(file_.parent_path());
        std::ofstream{file_, std::ios::binary} << content;
    }

    [[nodiscard]] std::string read() const
    {
        std::ifstream input(file_, std::ios::binary);
        std::ostringstream content;
        content << input.rdbuf();
        return content.str();
    }

    std::filesystem::path directory_;
    std::filesystem::path file_;
};

TEST_F(MetricsConfigTest, CreatesTheFileWithTheDefaults)
{
    const MetricsConfig config(file_, true);
    EXPECT_FALSE(config.didExistBefore());
    EXPECT_TRUE(config.isEnabled());
    EXPECT_FALSE(config.isLogErrorsEnabled());
    EXPECT_FALSE(config.isLogSentDataEnabled());
    EXPECT_FALSE(config.isLogResponseStatusTextEnabled());
    EXPECT_EQ(config.getServerUUID().size(), 36U);
    EXPECT_TRUE(std::filesystem::exists(file_));
}

TEST_F(MetricsConfigTest, WritesTheDefaultEnabledSetting)
{
    const MetricsConfig config(file_, false);
    EXPECT_FALSE(config.isEnabled());
    EXPECT_NE(read().find("enabled = false"), std::string::npos);
}

TEST_F(MetricsConfigTest, WritesFiveCommentLinesThenTheSettings)
{
    const MetricsConfig config(file_, true);
    std::istringstream lines(read());
    const auto next = [&lines] {
        std::string line;
        std::getline(lines, line);
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        return line;
    };
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(next().starts_with("# "));
    }
    EXPECT_EQ(next(), "enabled = true");
    EXPECT_EQ(next(), "server-uuid = \"" + config.getServerUUID() + "\"");
    EXPECT_EQ(next(), "log-errors = false");
    EXPECT_EQ(next(), "log-sent-data = false");
    EXPECT_EQ(next(), "log-response-status-text = false");
}

TEST_F(MetricsConfigTest, ReadsAnExistingFileAndKeepsItsUuid)
{
    write("enabled = false\nserver-uuid = \"abc\"\nlog-errors = true\nlog-sent-data = true\nlog-response-status-text = "
          "true\n");
    const MetricsConfig config(file_, true);
    EXPECT_TRUE(config.didExistBefore());
    EXPECT_FALSE(config.isEnabled());
    EXPECT_EQ(config.getServerUUID(), "abc");
    EXPECT_TRUE(config.isLogErrorsEnabled());
    EXPECT_TRUE(config.isLogSentDataEnabled());
    EXPECT_TRUE(config.isLogResponseStatusTextEnabled());
}

TEST_F(MetricsConfigTest, ReadsWindowsLineEndings)
{
    write("enabled = false\r\nserver-uuid = \"abc\"\r\n");
    const MetricsConfig config(file_, true);
    EXPECT_FALSE(config.isEnabled());
    EXPECT_EQ(config.getServerUUID(), "abc");
}

TEST_F(MetricsConfigTest, AMissingEnabledReadsAsTrue)
{
    write("server-uuid = \"abc\"\n");
    EXPECT_TRUE(MetricsConfig(file_, false).isEnabled());
    write("enabled = false\nserver-uuid = \"abc\"\n");
    EXPECT_FALSE(MetricsConfig(file_, true).isEnabled());
}

TEST_F(MetricsConfigTest, WritesAgainAFileWithoutAUuid)
{
    write("enabled = false\n");
    const MetricsConfig config(file_, true);
    EXPECT_TRUE(config.didExistBefore());
    EXPECT_TRUE(config.isEnabled());
    EXPECT_EQ(config.getServerUUID().size(), 36U);
    EXPECT_NE(read().find("server-uuid = "), std::string::npos);
}

TEST_F(MetricsConfigTest, TheUuidSurvivesAReread)
{
    const auto first = MetricsConfig(file_, true).getServerUUID();
    EXPECT_EQ(MetricsConfig(file_, true).getServerUUID(), first);
}

TEST_F(MetricsConfigTest, ThrowsWhenTheFileIsNotToml)
{
    write("enabled = = =\n");
    EXPECT_THROW(MetricsConfig(file_, true), std::exception);
}

TEST_F(MetricsConfigTest, IgnoresKeysItDoesNotKnow)
{
    write("server-uuid = \"abc\"\nsomething-else = 1\n");
    EXPECT_EQ(MetricsConfig(file_, true).getServerUUID(), "abc");
}

TEST_F(MetricsConfigTest, ThrowsWhenTheFileCannotBeWritten)
{
    write("not a directory");
    EXPECT_THROW(MetricsConfig(file_ / "nested" / "config.txt", true), std::exception);
}

}  // namespace
}  // namespace endgate
