#include "endgate/command/builtin/send.h"

#include <memory>
#include <string>

#include "endgate/brigadier/dispatcher.h"
#include "endgate/command/builtin/messages.h"
#include "endgate/command/builtin/util.h"
#include "endgate/command/manager.h"
#include "endgate/server.h"

namespace endgate {

namespace {

using brigadier::SINGLE_SUCCESS;
using brigadier::StringArgumentType;
using brigadier::SuggestionsBuilder;
using Context = brigadier::CommandContext<BrigadierCommand::Source>;

constexpr auto SERVER_ARG = "server";
constexpr auto PLAYER_ARG = "player";

}  // namespace

void SendCommand::registerCommand() const
{
    ProxyServer &proxy = server_;

    const auto usage = [](const Context &context) {
        context.getSource()->sendMessage("/send <player> <server>");
        return SINGLE_SUCCESS;
    };

    auto root_node = BrigadierCommand::literalArgumentBuilder("send");
    root_node
        .requires_([](const BrigadierCommand::Source &source) {
            return source->getPermissionValue("endgate.command.send") == Tristate::True;
        })
        .executes(usage);

    auto player_node =
        BrigadierCommand::requiredArgumentBuilder(PLAYER_ARG, StringArgumentType::word())
            .suggests([&proxy](const Context &context, SuggestionsBuilder &builder) {
                const std::string argument =
                    context.getArguments().contains(PLAYER_ARG) ? context.getArgument<std::string>(PLAYER_ARG) : "";
                for (const auto &player : proxy.getAllPlayers()) {
                    if (BuiltinCommandUtil::regionMatches(player->getUsername(), argument)) {
                        builder.suggest(player->getUsername());
                    }
                }
                if (BuiltinCommandUtil::regionMatches("all", argument)) {
                    builder.suggest("all");
                }
                if (BuiltinCommandUtil::regionMatches("current", argument) && context.getSource()->is<Player>()) {
                    builder.suggest("current");
                }
                return builder.build();
            })
            .executes(usage);

    const auto server_node =
        BrigadierCommand::requiredArgumentBuilder(SERVER_ARG, StringArgumentType::word())
            .suggests([&proxy](const Context &context, SuggestionsBuilder &builder) {
                const std::string argument =
                    context.getArguments().contains(SERVER_ARG) ? context.getArgument<std::string>(SERVER_ARG) : "";
                for (const BackendServer *backend : proxy.getAllServers()) {
                    const std::string &server_name = backend->getServerInfo().getName();
                    if (BuiltinCommandUtil::regionMatches(server_name, argument)) {
                        builder.suggest(server_name);
                    }
                }
                return builder.build();
            })
            .executes([&proxy](const Context &context) {
                const std::string server_name = context.getArgument<std::string>(SERVER_ARG);
                const std::string player = context.getArgument<std::string>(PLAYER_ARG);

                BackendServer *const target_server = proxy.getServer(server_name);
                if (target_server == nullptr) {
                    context.getSource()->sendMessage(CommandMessages::serverDoesNotExist(server_name));
                    return 0;
                }

                const std::shared_ptr<Player> maybe_player = proxy.getPlayer(player);
                if (!maybe_player && player != "all" && player != "current") {
                    context.getSource()->sendMessage(CommandMessages::playerNotFound(player));
                    return 0;
                }

                if (player == "all") {
                    for (const auto &p : proxy.getAllPlayers()) {
                        p->createConnectionRequest(*target_server).fireAndForget();
                    }
                    return SINGLE_SUCCESS;
                }

                if (player == "current") {
                    const auto *source = context.getSource()->as<Player>();
                    if (source == nullptr) {
                        context.getSource()->sendMessage(CommandMessages::PLAYERS_ONLY);
                        return 0;
                    }

                    const BackendServer *connected_server = source->getCurrentServer();
                    if (connected_server != nullptr) {
                        for (const auto &p : BuiltinCommandUtil::getPlayersConnected(proxy, *connected_server)) {
                            p->createConnectionRequest(*target_server).fireAndForget();
                        }
                        return SINGLE_SUCCESS;
                    }
                    return 0;
                }

                maybe_player->createConnectionRequest(*target_server).fireAndForget();
                return SINGLE_SUCCESS;
            })
            .build();
    player_node.then(server_node);
    root_node.then(player_node.build());

    const BrigadierCommand command(root_node);
    CommandManager &commands = proxy.getCommandManager();
    commands.registerCommand(CommandManager::metaBuilder(command).build(), command);
}

}  // namespace endgate
