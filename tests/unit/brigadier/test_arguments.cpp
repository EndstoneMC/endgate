#include <gtest/gtest.h>

#include "endgate/brigadier/arguments.h"

namespace endgate::brigadier {
namespace {

TEST(StringArgumentTypeTest, testParseWord)
{
    StringReader reader("hello world");
    EXPECT_EQ(std::get<std::string>(StringArgumentType::word()->parse(reader)), "hello");
    EXPECT_EQ(reader.getRead(), "hello");
}

TEST(StringArgumentTypeTest, testParseString)
{
    StringReader reader("\"hello world\" rest");
    EXPECT_EQ(std::get<std::string>(StringArgumentType::string()->parse(reader)), "hello world");
    EXPECT_EQ(reader.getRemaining(), " rest");
}

TEST(StringArgumentTypeTest, testParseGreedyString)
{
    StringReader reader("Hello world! This is a test.");
    EXPECT_EQ(std::get<std::string>(StringArgumentType::greedyString()->parse(reader)), "Hello world! This is a test.");
    EXPECT_FALSE(reader.canRead());
}

TEST(StringArgumentTypeTest, testToString)
{
    EXPECT_EQ(StringArgumentType::string()->toString(), "string()");
}

TEST(StringArgumentTypeTest, testEscapeIfRequiredNotRequired)
{
    EXPECT_EQ(StringArgumentType::escapeIfRequired("hello"), "hello");
    EXPECT_EQ(StringArgumentType::escapeIfRequired(""), "");
}

TEST(StringArgumentTypeTest, testEscapeIfRequiredMultipleWords)
{
    EXPECT_EQ(StringArgumentType::escapeIfRequired("hello world"), "\"hello world\"");
}

TEST(StringArgumentTypeTest, testEscapeIfRequiredQuote)
{
    EXPECT_EQ(StringArgumentType::escapeIfRequired("hello \"world\"!"), "\"hello \\\"world\\\"!\"");
}

TEST(StringArgumentTypeTest, testEscapeIfRequiredEscapes)
{
    EXPECT_EQ(StringArgumentType::escapeIfRequired("\\"), "\"\\\\\"");
}

TEST(StringArgumentTypeTest, testEscapeIfRequiredSingleQuote)
{
    EXPECT_EQ(StringArgumentType::escapeIfRequired("\""), "\"\\\"\"");
}

}  // namespace
}  // namespace endgate::brigadier
