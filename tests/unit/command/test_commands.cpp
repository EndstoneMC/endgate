#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/backend.h"
#include "endgate/command/builtin/endgate.h"
#include "endgate/command/builtin/glist.h"
#include "endgate/command/builtin/send.h"
#include "endgate/command/builtin/server.h"
#include "endgate/command/builtin/shutdown.h"
#include "endgate/connection/backend.h"
#include "endgate/connection/player.h"
#include "sources.h"
#include "support.h"

namespace endgate {
namespace {

class BuiltinTest : public testing::Test {
protected:
    void SetUp() override
    {
        server_.registerServer(ServerInfo::parse("lobby", "127.0.0.1:19132").value());
        server_.registerServer(ServerInfo::parse("survival", "127.0.0.1:19133").value());
        server_.registerServer(ServerInfo::parse("creative", "127.0.0.1:19134").value());

        CommandManager &commands = server_.getCommandManager();
        const auto endgate_command = EndgateCommand::create(server_);
        commands.registerCommand(CommandManager::metaBuilder(endgate_command).build(), endgate_command);
        const auto server_command = ServerCommand::create(server_);
        commands.registerCommand(CommandManager::metaBuilder(server_command).build(), server_command);
        const auto shutdown_command = ShutdownCommand::command(server_);
        commands.registerCommand(CommandManager::metaBuilder(shutdown_command).aliases({"end", "stop"}).build(),
                                 shutdown_command);
        GlistCommand(server_).registerCommand();
        SendCommand(server_).registerCommand();
    }

    bool run(const std::shared_ptr<CommandSource> &source, const std::string &line)
    {
        return server_.getCommandManager().executeImmediatelyAsync(source, line);
    }

    std::vector<std::string> suggest(const std::shared_ptr<CommandSource> &source, const std::string &line)
    {
        return server_.getCommandManager().offerSuggestions(source, line);
    }

    [[nodiscard]] std::shared_ptr<Player> join(const std::string &name, const std::string &backend = {})
    {
        const auto connection =
            std::make_shared<TestConnection>(node_.endpoint(), "127.0.0.1:1", Direction::Serverbound);
        connection_ = connection;
        const auto player = std::make_shared<Player>(
            server_, connection, GameProfile{.id = UuidUtils::generateOfflinePlayerUuid(name), .name = name});
        EXPECT_TRUE(server_.registerConnection(player));
        if (!backend.empty()) {
            player->setConnectedServer(std::make_shared<ServerConnection>(*server_.getServer(backend), player));
        }
        return player;
    }

    ProxyServer server_;
    std::shared_ptr<TestConnection> connection_;
    Registry registry_;
    Node node_{server_, registry_};
    std::shared_ptr<TestSource> source_ = std::make_shared<TestSource>();
};

TEST_F(BuiltinTest, AnEmptyPermissionSetSeesNoBuiltinExceptServer)
{
    const auto nobody = std::make_shared<TestSource>(Tristate::Undefined);
    const auto player = join("Steve");

    for (const char *alias : {"endgate", "glist", "send", "shutdown", "end", "stop"}) {
        EXPECT_FALSE(server_.getCommandManager().hasCommand(alias, nobody)) << alias;
        EXPECT_FALSE(server_.getCommandManager().hasCommand(alias, player)) << alias;
    }
    EXPECT_FALSE(server_.getCommandManager().hasCommand("server", nobody));
    EXPECT_TRUE(server_.getCommandManager().hasCommand("server", player));
}

TEST_F(BuiltinTest, AServerPermissionOfFalseTakesServerAwayFromAPlayer)
{
    class Denied final : public CommandSource {
    public:
        void sendMessage(std::string_view) override {}
        [[nodiscard]] Tristate getPermissionValue(const std::string &) const override { return Tristate::False; }
    };
    EXPECT_FALSE(server_.getCommandManager().hasCommand("server", std::make_shared<Denied>()));
}

TEST_F(BuiltinTest, OnlyTheConsoleMayShutDown)
{
    const auto console = server_.getConsoleCommandSource().shared_from_this();

    EXPECT_TRUE(server_.getCommandManager().hasCommand("shutdown", console));
    EXPECT_TRUE(server_.getCommandManager().hasCommand("end", console));
    EXPECT_TRUE(server_.getCommandManager().hasCommand("stop", console));
    EXPECT_FALSE(server_.getCommandManager().hasCommand("shutdown", source_));
    EXPECT_FALSE(run(source_, "shutdown"));
    EXPECT_FALSE(server_.isShuttingDown());
}

TEST_F(BuiltinTest, ShutdownTakesAGreedyReason)
{
    const auto shutdown = server_.getCommandManager().getCommandMeta("shutdown");
    ASSERT_TRUE(shutdown.has_value());
    EXPECT_EQ(shutdown->getAliases(), (std::vector<std::string>{"shutdown", "end", "stop"}));
    EXPECT_EQ(suggest(server_.getConsoleCommandSource().shared_from_this(), "sh"),
              (std::vector<std::string>{"shutdown"}));
}

TEST_F(BuiltinTest, GlistCountsPlayers)
{
    EXPECT_TRUE(run(source_, "glist"));
    EXPECT_EQ(source_->messages, (std::vector<std::string>{"0 players are currently connected to the proxy.",
                                                           "To view all players on servers, use /glist all."}));

    source_->messages.clear();
    const auto steve = join("Steve", "lobby");
    EXPECT_TRUE(run(source_, "glist"));
    EXPECT_EQ(source_->messages.front(), "1 player is currently connected to the proxy.");
}

TEST_F(BuiltinTest, GlistAllListsEachServerWithPlayersInOrder)
{
    const auto steve = join("Steve", "survival");
    const auto alex = join("Alex", "survival");
    const auto kai = join("Kai", "lobby");

    EXPECT_TRUE(run(source_, "glist all"));

    ASSERT_EQ(source_->messages.size(), 3U);
    EXPECT_EQ(source_->messages[0], "[lobby] (1): Kai");
    EXPECT_TRUE(source_->messages[1] == "[survival] (2): Steve, Alex" ||
                source_->messages[1] == "[survival] (2): Alex, Steve");
    EXPECT_EQ(source_->messages[2], "3 players are currently connected to the proxy.");
}

TEST_F(BuiltinTest, GlistOfOneServerListsItEvenWhenEmpty)
{
    const auto steve = join("Steve", "lobby");

    EXPECT_TRUE(run(source_, "glist creative"));
    EXPECT_TRUE(run(source_, "glist LOBBY"));
    EXPECT_TRUE(run(source_, "glist nowhere"));

    EXPECT_EQ(source_->messages,
              (std::vector<std::string>{"[creative] (0): ", "The specified server LOBBY does not exist.",
                                        "The specified server nowhere does not exist."}));
}

TEST_F(BuiltinTest, GlistSuggestsServersAndAll)
{
    EXPECT_EQ(suggest(source_, "glist "), (std::vector<std::string>{"all", "creative", "lobby", "survival"}));
    EXPECT_EQ(suggest(source_, "glist S"), (std::vector<std::string>{"survival"}));
    EXPECT_EQ(suggest(source_, "glist A"), (std::vector<std::string>{"all"}));
}

TEST_F(BuiltinTest, SendNeedsAPlayerAndAServer)
{
    EXPECT_TRUE(run(source_, "send"));
    EXPECT_TRUE(run(source_, "send Steve"));
    EXPECT_EQ(source_->messages, (std::vector<std::string>{"/send <player> <server>", "/send <player> <server>"}));
}

TEST_F(BuiltinTest, SendReportsAMissingServerOrPlayer)
{
    const auto steve = join("Steve", "lobby");
    EXPECT_TRUE(run(source_, "send Steve nowhere"));
    EXPECT_TRUE(run(source_, "send Nobody lobby"));
    EXPECT_TRUE(run(source_, "send current lobby"));

    EXPECT_EQ(source_->messages, (std::vector<std::string>{"The specified server nowhere does not exist.",
                                                           "The specified player Nobody does not exist.",
                                                           "Only players can run this command."}));
}

TEST_F(BuiltinTest, SendSuggestsPlayersTargetsAndServers)
{
    const auto steve = join("Steve");
    const auto alex = join("Alex");

    const auto from_console = suggest(source_, "send ");
    EXPECT_EQ(std::count(from_console.begin(), from_console.end(), "Steve"), 1);
    EXPECT_EQ(std::count(from_console.begin(), from_console.end(), "Alex"), 1);
    EXPECT_EQ(std::count(from_console.begin(), from_console.end(), "all"), 1);
    EXPECT_EQ(std::count(from_console.begin(), from_console.end(), "current"), 0);
    EXPECT_EQ(suggest(source_, "send Steve "), (std::vector<std::string>{"creative", "lobby", "survival"}));
    EXPECT_EQ(suggest(source_, "send Steve su"), (std::vector<std::string>{"survival"}));
}

TEST_F(BuiltinTest, ServerShowsTheCurrentServerAndTheOthers)
{
    const auto steve = join("Steve", "lobby");
    const auto connection = connection_;

    EXPECT_TRUE(node_.call([&] { return run(steve, "server"); }));
    EXPECT_EQ(connection->sent(), 2U);
}

TEST_F(BuiltinTest, ServerWithAnUnknownNameSaysSo)
{
    const auto steve = join("Steve", "lobby");
    const auto connection = connection_;

    EXPECT_TRUE(node_.call([&] { return run(steve, "server nowhere"); }));
    EXPECT_EQ(connection->sent(), 1U);
}

TEST_F(BuiltinTest, ServerIsUnknownToTheConsole)
{
    EXPECT_FALSE(run(server_.getConsoleCommandSource().shared_from_this(), "server"));
}

TEST_F(BuiltinTest, ServerSuggestsServers)
{
    const auto steve = join("Steve", "lobby");

    EXPECT_EQ(suggest(steve, "server "), (std::vector<std::string>{"creative", "lobby", "survival"}));
    EXPECT_EQ(suggest(steve, "server L"), (std::vector<std::string>{"lobby"}));
}

TEST_F(BuiltinTest, EndgateInfoPrintsTheVersionAndTheCopyright)
{
    EXPECT_TRUE(run(source_, "endgate info"));

    ASSERT_EQ(source_->messages.size(), 2U);
    EXPECT_EQ(source_->messages[0], std::string("Endgate ") + ENDGATE_VERSION);
    EXPECT_NE(source_->messages[1].find("Endgate is licensed under the terms of"), std::string::npos);
}

TEST_F(BuiltinTest, EndgateAloneListsWhatTheSourceMayRun)
{
    EXPECT_TRUE(run(source_, "endgate"));
    source_->permissions["endgate.command.reload"] = Tristate::False;
    EXPECT_TRUE(run(source_, "endgate"));
    source_->permissions["endgate.command.info"] = Tristate::False;
    EXPECT_FALSE(run(source_, "endgate"));

    EXPECT_EQ(source_->messages, (std::vector<std::string>{"/endgate <info|reload>", "/endgate <info>"}));
}

TEST_F(BuiltinTest, EndgateSuggestsItsSubcommands)
{
    EXPECT_EQ(suggest(source_, "endgate "), (std::vector<std::string>{"info", "reload"}));
}

}  // namespace
}  // namespace endgate
