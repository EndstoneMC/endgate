#include <gtest/gtest.h>

#include "endgate/brigadier/exceptions.h"

namespace endgate::brigadier {
namespace {

TEST(DynamicCommandExceptionTypeTest, createWithContext)
{
    const DynamicCommandExceptionType type([](const std::string &name) { return "Hello, " + name + "!"; });
    StringReader reader("Foo bar");
    reader.setCursor(5);
    const CommandSyntaxException exception = type.createWithContext(reader, "World");
    EXPECT_EQ(&exception.getType(), &type);
    EXPECT_EQ(exception.getInput(), "Foo bar");
    EXPECT_EQ(exception.getCursor(), 5);
    EXPECT_EQ(exception.getRawMessage(), "Hello, World!");
}

TEST(SimpleCommandExceptionTypeTest, createWithContext)
{
    const SimpleCommandExceptionType type("error");
    StringReader reader("Foo bar");
    reader.setCursor(5);
    const CommandSyntaxException exception = type.createWithContext(reader);
    EXPECT_EQ(&exception.getType(), &type);
    EXPECT_EQ(exception.getInput(), "Foo bar");
    EXPECT_EQ(exception.getCursor(), 5);
}

TEST(SimpleCommandExceptionTypeTest, getContextNone)
{
    const CommandExceptionType type;
    const CommandSyntaxException exception(type, "error");
    EXPECT_FALSE(exception.getContext().has_value());
    EXPECT_STREQ(exception.what(), "error");
}

TEST(SimpleCommandExceptionTypeTest, getContextShort)
{
    const CommandExceptionType type;
    const CommandSyntaxException exception(type, "error", "Hello world!", 5);
    EXPECT_EQ(exception.getContext(), "Hello<--[HERE]");
    EXPECT_STREQ(exception.what(), "error at position 5: Hello<--[HERE]");
}

TEST(SimpleCommandExceptionTypeTest, getContextLong)
{
    const CommandExceptionType type;
    const CommandSyntaxException exception(type, "error", "Hello world! This has an error in it. Oh dear!", 20);
    EXPECT_EQ(exception.getContext(), "...d! This ha<--[HERE]");
}

}  // namespace
}  // namespace endgate::brigadier
