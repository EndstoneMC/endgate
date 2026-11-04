#include "endgate/command/builtin/glist.h"

#include <format>
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

}  // namespace

void GlistCommand::registerCommand() const
{
    ProxyServer &proxy = server_;

    const auto send_total_proxy_count = [&proxy](CommandSource &target) {
        const std::size_t online = proxy.getAllPlayers().size();
        target.sendMessage(
            std::format("{} {} currently connected to the proxy.", online, online == 1 ? "player is" : "players are"));
    };

    const auto send_server_players = [&proxy](CommandSource &target, const BackendServer &backend, bool from_all) {
        const auto on_server = BuiltinCommandUtil::getPlayersConnected(proxy, backend);
        if (on_server.empty() && from_all) {
            return;
        }

        std::string message = std::format("[{}] ({}): ", backend.getServerInfo().getName(), on_server.size());
        for (std::size_t i = 0; i < on_server.size(); i++) {
            message += on_server[i]->getUsername();
            if (i + 1 < on_server.size()) {
                message += ", ";
            }
        }
        target.sendMessage(message);
    };

    auto root_node = BrigadierCommand::literalArgumentBuilder("glist");
    root_node
        .requires_([](const BrigadierCommand::Source &source) {
            return source->getPermissionValue("endgate.command.glist") == Tristate::True;
        })
        .executes([send_total_proxy_count](const Context &context) {
            CommandSource &source = *context.getSource();
            send_total_proxy_count(source);
            source.sendMessage("To view all players on servers, use /glist all.");
            return 1;
        });

    auto server_node = BrigadierCommand::requiredArgumentBuilder(SERVER_ARG, StringArgumentType::string())
                           .suggests([&proxy](const Context &context, SuggestionsBuilder &builder) {
                               const std::string argument = context.getArguments().contains(SERVER_ARG)
                                                              ? context.getArgument<std::string>(SERVER_ARG)
                                                              : "";
                               for (const BackendServer *backend : proxy.getAllServers()) {
                                   const std::string &server_name = backend->getServerInfo().getName();
                                   if (BuiltinCommandUtil::regionMatches(server_name, argument)) {
                                       builder.suggest(server_name);
                                   }
                               }
                               if (BuiltinCommandUtil::regionMatches("all", argument)) {
                                   builder.suggest("all");
                               }
                               return builder.build();
                           })
                           .executes([&proxy, send_total_proxy_count, send_server_players](const Context &context) {
                               CommandSource &source = *context.getSource();
                               const std::string server_name = StringArgumentType::getString(context, SERVER_ARG);
                               if (BuiltinCommandUtil::equalsIgnoreCase(server_name, "all")) {
                                   for (const BackendServer *backend : BuiltinCommandUtil::sortedServerList(proxy)) {
                                       send_server_players(source, *backend, true);
                                   }
                                   send_total_proxy_count(source);
                               }
                               else {
                                   const BackendServer *registered_server = proxy.getServer(server_name);
                                   if (registered_server == nullptr) {
                                       source.sendErrorMessage(CommandMessages::serverDoesNotExist(server_name));
                                       return -1;
                                   }
                                   send_server_players(source, *registered_server, false);
                               }
                               return SINGLE_SUCCESS;
                           });
    root_node.then(server_node);

    const BrigadierCommand command(root_node);
    CommandManager &commands = proxy.getCommandManager();
    commands.registerCommand(CommandManager::metaBuilder(command).build(), command);
}

}  // namespace endgate
