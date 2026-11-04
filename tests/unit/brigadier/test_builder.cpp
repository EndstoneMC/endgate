#include <gtest/gtest.h>

#include "endgate/brigadier/dispatcher.h"

namespace endgate::brigadier {
namespace {

using Literal = LiteralArgumentBuilder<int>;
using Required = RequiredArgumentBuilder<int>;

class TestableArgumentBuilder : public ArgumentBuilder<int, TestableArgumentBuilder> {};

TEST(ArgumentBuilderTest, testArguments)
{
    TestableArgumentBuilder builder;
    const Required argument = Required::argument("bar", StringArgumentType::word());
    builder.then(argument);
    ASSERT_EQ(builder.getArguments().size(), 1);
    EXPECT_EQ(builder.getArguments().front()->getName(), "bar");
}

class LiteralArgumentBuilderTest : public ::testing::Test {
protected:
    Literal builder_ = Literal::literal("foo");
    Command<int> command_ = [](const CommandContext<int> &) {
        return 1;
    };
};

TEST_F(LiteralArgumentBuilderTest, testBuild)
{
    EXPECT_EQ(builder_.build()->getLiteral(), "foo");
}

TEST_F(LiteralArgumentBuilderTest, testBuildWithExecutor)
{
    const auto node = builder_.executes(command_).build();
    EXPECT_EQ(node->getLiteral(), "foo");
    EXPECT_TRUE(node->getCommand());
}

TEST_F(LiteralArgumentBuilderTest, testBuildWithChildren)
{
    builder_.then(Required::argument("bar", StringArgumentType::word()));
    builder_.then(Required::argument("baz", StringArgumentType::word()));
    EXPECT_EQ(builder_.build()->getChildren().size(), 2);
}

class RequiredArgumentBuilderTest : public ::testing::Test {
protected:
    std::shared_ptr<const ArgumentType> type_ = StringArgumentType::word();
    Required builder_ = Required::argument("foo", type_);
    Command<int> command_ = [](const CommandContext<int> &) {
        return 1;
    };
};

TEST_F(RequiredArgumentBuilderTest, testBuild)
{
    const auto node = builder_.build();
    EXPECT_EQ(node->getName(), "foo");
    EXPECT_EQ(node->getType(), type_);
}

TEST_F(RequiredArgumentBuilderTest, testBuildWithExecutor)
{
    const auto node = builder_.executes(command_).build();
    EXPECT_EQ(node->getName(), "foo");
    EXPECT_EQ(node->getType(), type_);
    EXPECT_TRUE(node->getCommand());
}

TEST_F(RequiredArgumentBuilderTest, testBuildWithChildren)
{
    builder_.then(Required::argument("bar", StringArgumentType::word()));
    builder_.then(Required::argument("baz", StringArgumentType::word()));
    EXPECT_EQ(builder_.build()->getChildren().size(), 2);
}

}  // namespace
}  // namespace endgate::brigadier
