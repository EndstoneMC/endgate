#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/command/manager.h"
#include "sources.h"

namespace endgate {
namespace {

using brigadier::SINGLE_SUCCESS;
using brigadier::StringArgumentType;
using Context = brigadier::CommandContext<BrigadierCommand::Source>;

class ManagerTest : public testing::Test {
protected:
    [[nodiscard]] BrigadierCommand echo(const std::string &name)
    {
        return BrigadierCommand(BrigadierCommand::literalArgumentBuilder(name)
                                    .executes([this](const Context &) {
                                        runs_++;
                                        return SINGLE_SUCCESS;
                                    })
                                    .then(BrigadierCommand::requiredArgumentBuilder("word", StringArgumentType::word())
                                              .executes([this](const Context &context) {
                                                  context.getSource()->sendMessage(
                                                      context.getArgument<std::string>("word"));
                                                  return SINGLE_SUCCESS;
                                              })));
    }

    CommandManager manager_;
    std::shared_ptr<TestSource> source_ = std::make_shared<TestSource>();
    int runs_ = 0;
};

TEST_F(ManagerTest, ARegisteredCommandRunsAndAnUnknownOneDoesNot)
{
    manager_.registerCommand(echo("echo"));

    EXPECT_TRUE(manager_.executeImmediatelyAsync(source_, "echo"));
    EXPECT_EQ(runs_, 1);
    EXPECT_FALSE(manager_.executeImmediatelyAsync(source_, "nothing"));
}

TEST_F(ManagerTest, TheAliasIsCaseInsensitiveAndTheArgumentsAreNot)
{
    manager_.registerCommand(echo("echo"));

    EXPECT_TRUE(manager_.executeImmediatelyAsync(source_, "  EcHo Hello "));
    EXPECT_EQ(source_->messages, (std::vector<std::string>{"Hello"}));
}

TEST_F(ManagerTest, AMetaAliasRunsTheSameCommand)
{
    const BrigadierCommand command = echo("echo");
    manager_.registerCommand(CommandManager::metaBuilder(command).aliases({"Say", "shout"}).build(), command);

    EXPECT_TRUE(manager_.executeImmediatelyAsync(source_, "say hi"));
    EXPECT_TRUE(manager_.executeImmediatelyAsync(source_, "shout"));
    EXPECT_EQ(source_->messages, (std::vector<std::string>{"hi"}));
    EXPECT_EQ(runs_, 1);
    EXPECT_EQ(manager_.getAliases(), (std::vector<std::string>{"echo", "say", "shout"}));
}

TEST_F(ManagerTest, ASyntaxErrorIsReportedAndCountsAsRun)
{
    manager_.registerCommand(echo("echo"));

    EXPECT_TRUE(manager_.executeImmediatelyAsync(source_, "echo a b"));
    EXPECT_EQ(source_->messages.size(), 1U);
}

TEST_F(ManagerTest, ACommandTheSourceMayNotUseIsUnknownToIt)
{
    manager_.registerCommand(BrigadierCommand(
        BrigadierCommand::literalArgumentBuilder("secret")
            .requires_([](const BrigadierCommand::Source &source) { return source->hasPermission("secret"); })
            .executes([](const Context &) { return SINGLE_SUCCESS; })));

    EXPECT_TRUE(manager_.executeImmediatelyAsync(source_, "secret"));
    EXPECT_TRUE(manager_.hasCommand("secret"));
    EXPECT_TRUE(manager_.hasCommand("secret", source_));
    const auto denied = std::make_shared<TestSource>(Tristate::Undefined);
    EXPECT_FALSE(manager_.executeImmediatelyAsync(denied, "secret"));
    EXPECT_FALSE(manager_.hasCommand("secret", denied));
}

TEST_F(ManagerTest, ACommandReturningForwardIsNotAnExecutedCommand)
{
    manager_.registerCommand(
        BrigadierCommand(BrigadierCommand::literalArgumentBuilder("forwarded").executes([](const Context &) {
            return BrigadierCommand::FORWARD;
        })));

    EXPECT_FALSE(manager_.executeImmediatelyAsync(source_, "forwarded"));
}

TEST_F(ManagerTest, UnregisteringRemovesTheAliasAndItsMeta)
{
    const BrigadierCommand command = echo("echo");
    const CommandMeta meta = CommandManager::metaBuilder(command).aliases({"say"}).build();
    manager_.registerCommand(meta, command);
    EXPECT_EQ(manager_.getCommandMeta("say"), meta);

    manager_.unregister("say");

    EXPECT_FALSE(manager_.hasCommand("say"));
    EXPECT_FALSE(manager_.getCommandMeta("say").has_value());
    EXPECT_TRUE(manager_.hasCommand("echo"));

    manager_.unregister(meta);
    EXPECT_FALSE(manager_.hasCommand("echo"));
}

TEST_F(ManagerTest, RegisteringAgainReplacesTheEarlierCommand)
{
    manager_.registerCommand(echo("echo"));
    manager_.registerCommand(
        BrigadierCommand(BrigadierCommand::literalArgumentBuilder("echo").executes([this](const Context &) {
            runs_ += 10;
            return SINGLE_SUCCESS;
        })));

    EXPECT_TRUE(manager_.executeImmediatelyAsync(source_, "echo"));
    EXPECT_EQ(runs_, 10);
}

TEST_F(ManagerTest, AliasesAreSuggestedByPrefixAndOnlyIfTheSourceMayUseThem)
{
    manager_.registerCommand(echo("echo"));
    manager_.registerCommand(echo("end"));
    manager_.registerCommand(BrigadierCommand(
        BrigadierCommand::literalArgumentBuilder("exit")
            .requires_([](const BrigadierCommand::Source &source) { return source->hasPermission("exit"); })
            .executes([](const Context &) { return SINGLE_SUCCESS; })));

    EXPECT_EQ(manager_.offerSuggestions(source_, "e"), (std::vector<std::string>{"echo", "end", "exit"}));
    EXPECT_EQ(manager_.offerSuggestions(source_, "EC"), (std::vector<std::string>{"echo"}));
    EXPECT_EQ(manager_.offerSuggestions(std::make_shared<TestSource>(Tristate::Undefined), "e"),
              (std::vector<std::string>{"echo", "end"}));
    EXPECT_TRUE(manager_.offerSuggestions(source_, "missing a").empty());
}

TEST_F(ManagerTest, ArgumentsAreSuggestedByTheirProvider)
{
    manager_.registerCommand(BrigadierCommand(BrigadierCommand::literalArgumentBuilder("pick").then(
        BrigadierCommand::requiredArgumentBuilder("word", StringArgumentType::word())
            .suggests([](const Context &, brigadier::SuggestionsBuilder &builder) {
                builder.suggest("alpha").suggest("beta");
                return builder.build();
            }))));

    EXPECT_EQ(manager_.offerSuggestions(source_, "pick "), (std::vector<std::string>{"alpha", "beta"}));
    EXPECT_EQ(manager_.offerSuggestions(source_, "pick al"), (std::vector<std::string>{"alpha", "beta"}));
}

}  // namespace
}  // namespace endgate
