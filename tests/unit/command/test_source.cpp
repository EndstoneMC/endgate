#include <memory>

#include <gtest/gtest.h>

#include "endgate/connection/player.h"
#include "endgate/console.h"
#include "endgate/server.h"
#include "support.h"

namespace endgate {
namespace {

class SourceTest : public testing::Test {
protected:
    [[nodiscard]] std::shared_ptr<TestConnection> connect()
    {
        return std::make_shared<TestConnection>(node_.endpoint(), "127.0.0.1:1", Direction::Serverbound);
    }

    ProxyServer server_;
    Registry registry_;
    Node node_{server_, registry_};
};

TEST_F(SourceTest, APlayerIsAPlayerAndASourceButNotTheConsole)
{
    const auto player = std::make_shared<Player>(server_, connect(), GameProfile{.name = "Steve"});
    CommandSource &source = *player;

    EXPECT_TRUE(source.is<Player>());
    EXPECT_TRUE(source.is<CommandSource>());
    EXPECT_FALSE(source.is<Console>());
    EXPECT_EQ(source.as<Player>(), player.get());
    EXPECT_EQ(source.as<CommandSource>(), &source);
    EXPECT_EQ(source.as<Console>(), nullptr);
}

TEST_F(SourceTest, TheConsoleIsTheConsoleAndASourceButNotAPlayer)
{
    Console &console = server_.getConsoleCommandSource();
    CommandSource &source = console;

    EXPECT_TRUE(source.is<Console>());
    EXPECT_TRUE(source.is<CommandSource>());
    EXPECT_FALSE(source.is<Player>());
    EXPECT_EQ(source.as<Console>(), &console);
    EXPECT_EQ(source.as<Player>(), nullptr);
}

TEST_F(SourceTest, AConstSourceCastsToAConstType)
{
    const auto player = std::make_shared<Player>(server_, connect(), GameProfile{.name = "Steve"});
    const CommandSource &source = *player;

    EXPECT_EQ(source.as<Player>(), player.get());
    EXPECT_EQ(source.as<Console>(), nullptr);
}

TEST_F(SourceTest, TheConsoleHasEveryPermissionAndAPlayerNone)
{
    const auto player = std::make_shared<Player>(server_, connect(), GameProfile{.name = "Steve"});

    EXPECT_EQ(server_.getConsoleCommandSource().getPermissionValue("endgate.command.glist"), Tristate::True);
    EXPECT_EQ(player->getPermissionValue("endgate.command.server"), Tristate::Undefined);
    EXPECT_FALSE(player->hasPermission("endgate.command.server"));
}

}  // namespace
}  // namespace endgate
