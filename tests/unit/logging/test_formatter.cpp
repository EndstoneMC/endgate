#include <string>

#include <gtest/gtest.h>
#include <spdlog/pattern_formatter.h>

#include "endgate/logging/formatter.h"

namespace endgate {
namespace {

std::string colour(const std::string &message)
{
    return MinecraftFormattingConverter::convert(message, false);
}

std::string strip(const std::string &message)
{
    return MinecraftFormattingConverter::convert(message, true);
}

TEST(MinecraftFormattingConverter, MapsEachColourCodeToAnsi)
{
    EXPECT_EQ(colour("§cx"), "\x1b[91mx\x1b[0m");
    EXPECT_EQ(colour("§ax"), "\x1b[92mx\x1b[0m");
    EXPECT_EQ(colour("§ex"), "\x1b[93mx\x1b[0m");
    EXPECT_EQ(colour("§7x"), "\x1b[37mx\x1b[0m");
    EXPECT_EQ(colour("§3x"), "\x1b[36mx\x1b[0m");
    EXPECT_EQ(colour("§0x"), "\x1b[30mx\x1b[0m");
    EXPECT_EQ(colour("§8x"), "\x1b[90mx\x1b[0m");
    EXPECT_EQ(colour("§gx"), "\x1b[38;2;221;214;5mx\x1b[0m");
    EXPECT_EQ(colour("§vx"), "\x1b[38;2;234;113;19mx\x1b[0m");
}

TEST(MinecraftFormattingConverter, MapsFormattingCodesToAnsi)
{
    EXPECT_EQ(colour("§lx"), "\x1b[1mx\x1b[0m");
    EXPECT_EQ(colour("§ox"), "\x1b[3mx\x1b[0m");
    EXPECT_EQ(colour("§kx"), "\x1b[8mx\x1b[0m");
    EXPECT_EQ(colour("a§rb"), "a\x1b[0mb\x1b[0m");
}

TEST(MinecraftFormattingConverter, IgnoresTheCaseOfACode)
{
    EXPECT_EQ(colour("§Cx"), colour("§cx"));
}

TEST(MinecraftFormattingConverter, ResetsAtTheEndOfAColouredLine)
{
    EXPECT_EQ(colour("§3§lEndgate §r§3 1.0"), "\x1b[36m\x1b[1mEndgate \x1b[0m\x1b[36m 1.0\x1b[0m");
}

TEST(MinecraftFormattingConverter, LeavesAPlainLineAlone)
{
    EXPECT_EQ(colour("plain"), "plain");
    EXPECT_EQ(colour(""), "");
}

TEST(MinecraftFormattingConverter, KeepsAnUnknownCodeAndATrailingSectionSign)
{
    EXPECT_EQ(colour("§zx"), "§zx");
    EXPECT_EQ(colour("x§"), "x§");
    EXPECT_EQ(strip("§zx"), "§zx");
    EXPECT_EQ(strip("x§"), "x§");
}

TEST(MinecraftFormattingConverter, StripsEveryCode)
{
    EXPECT_EQ(strip("§3§lEndgate §r§3 1.0"), "Endgate  1.0");
    EXPECT_EQ(strip("§cOnly players can run this command."), "Only players can run this command.");
}

TEST(MinecraftFormattingConverter, StripsAnsiSequencesOnlyInStripMode)
{
    EXPECT_EQ(strip("\x1b[1;31mx\x1b[0m"), "x");
    EXPECT_EQ(colour("\x1b[31mx"), "\x1b[31mx");
}

TEST(MinecraftFormattingConverter, ReplacesTheMessageFlagOfAPattern)
{
    spdlog::pattern_formatter formatter(spdlog::pattern_time_type::local, "");
    formatter.add_flag<MinecraftFormattingConverter>('v', false);
    formatter.set_pattern("[%v]");

    spdlog::details::log_msg msg("name", spdlog::level::info, "§cx");
    spdlog::memory_buf_t formatted;
    formatter.format(msg, formatted);

    EXPECT_EQ(std::string(formatted.data(), formatted.size()), "[\x1b[91mx\x1b[0m]");
}

}  // namespace
}  // namespace endgate
