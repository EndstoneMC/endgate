#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "endgate/backend.h"
#include "endgate/command/brigadier.h"
#include "endgate/connection/backend.h"
#include "endgate/connection/player.h"
#include "support.h"

namespace endgate {
namespace {

constexpr int PROTOCOL = MAXIMUM_VERSION.getProtocol();

using brigadier::SINGLE_SUCCESS;
using Context = brigadier::CommandContext<BrigadierCommand::Source>;

class CommandRequestTest : public testing::Test {
protected:
    void SetUp() override
    {
        client_ = std::make_shared<TestConnection>(node_.endpoint(), "127.0.0.1:1", Direction::Serverbound);
        player_ = std::make_shared<Player>(server_, client_);
        handler_ = std::make_shared<ClientPlaySessionHandler>(player_);
        server_connection_ = std::make_shared<ServerConnection>(backend_server_, player_);
        node_.call([&] {
            client_->setProtocolVersion(MAXIMUM_VERSION);
            client_->setSessionHandler(handler_);
            player_->setConnectedServer(server_connection_);
            return 0;
        });
    }

    void registerCommand(const std::string &name, int result = SINGLE_SUCCESS)
    {
        server_.getCommandManager().registerCommand(
            BrigadierCommand(BrigadierCommand::literalArgumentBuilder(name).executes([this, result](const Context &) {
                runs_++;
                return result;
            })));
    }

    [[nodiscard]] bool request(const std::string &command)
    {
        PacketOf<PROTOCOL, bp::MinecraftPacketIds::CommandRequest> packet;
        packet.command = command;
        return node_.call([&] { return handler_->handle(packet); });
    }

    ProxyServer server_;
    Registry registry_;
    Node node_{server_, registry_};
    BackendServer backend_server_{ServerInfo::parse("lobby", "127.0.0.1:19132").value()};
    std::shared_ptr<TestConnection> client_;
    std::shared_ptr<Player> player_;
    std::shared_ptr<ClientPlaySessionHandler> handler_;
    std::shared_ptr<ServerConnection> server_connection_;
    int runs_ = 0;
};

TEST_F(CommandRequestTest, AProxyCommandRunsOnTheProxyWithOrWithoutTheSlash)
{
    registerCommand("hop");

    EXPECT_TRUE(request("/hop"));
    EXPECT_TRUE(request("hop"));
    EXPECT_TRUE(request("/HOP"));
    EXPECT_EQ(runs_, 3);
}

TEST_F(CommandRequestTest, ACommandNoProxyCommandNamesGoesToTheBackend)
{
    registerCommand("hop");

    EXPECT_FALSE(request("/tp @s 0 0 0"));
    EXPECT_FALSE(request("say hop"));
    EXPECT_FALSE(request("/"));
    EXPECT_FALSE(request(""));
    EXPECT_EQ(runs_, 0);
}

TEST_F(CommandRequestTest, OnlyTheFirstWordNamesTheCommand)
{
    registerCommand("hop");

    EXPECT_FALSE(request("/say /hop"));
    EXPECT_EQ(runs_, 0);
}

TEST_F(CommandRequestTest, ACommandThatForwardsGoesToTheBackend)
{
    registerCommand("hop", BrigadierCommand::FORWARD);

    EXPECT_FALSE(request("/hop"));
    EXPECT_EQ(runs_, 1);
}

TEST_F(CommandRequestTest, ACommandThePlayerMayNotUseGoesToTheBackend)
{
    server_.getCommandManager().registerCommand(BrigadierCommand(
        BrigadierCommand::literalArgumentBuilder("glist")
            .requires_([](const BrigadierCommand::Source &source) { return source->hasPermission("glist"); })
            .executes([this](const Context &) {
                runs_++;
                return SINGLE_SUCCESS;
            })));

    EXPECT_FALSE(request("/glist"));
    EXPECT_EQ(runs_, 0);
}

TEST_F(CommandRequestTest, ACommandWithNoServerToSendItToIsDropped)
{
    registerCommand("hop");
    node_.call([&] {
        player_->setConnectedServer(nullptr);
        return 0;
    });

    EXPECT_TRUE(request("/tp @s 0 0 0"));
    EXPECT_TRUE(request("/hop"));
    EXPECT_EQ(runs_, 0);
}

}  // namespace
}  // namespace endgate
