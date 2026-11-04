#include "endgate/command/builtin/server.h"

#include <algorithm>
#include <format>
#include <string>

#include "endgate/brigadier/dispatcher.h"
#include "endgate/command/builtin/messages.h"
#include "endgate/command/builtin/util.h"
#include "endgate/server.h"

namespace endgate {

namespace {

using brigadier::SINGLE_SUCCESS;
using brigadier::StringArgumentType;
using brigadier::SuggestionsBuilder;
using Context = brigadier::CommandContext<BrigadierCommand::Source>;

constexpr auto SERVER_ARG = "server";

}  // namespace

BrigadierCommand ServerCommand::create(ProxyServer &server)
{
    const auto node =
        BrigadierCommand::literalArgumentBuilder("server")
            .requires_([](const BrigadierCommand::Source &source) {
                return source->is<Player>() && source->getPermissionValue("endgate.command.server") != Tristate::False;
            })
            .executes([&server](const Context &context) {
                auto &player = *context.getSource()->as<Player>();
                const BackendServer *current = player.getCurrentServer();
                const std::string current_server =
                    current != nullptr ? current->getServerInfo().getName() : "<unknown>";
                player.sendMessage(std::format("You are currently connected to {}.", current_server));

                const auto servers = BuiltinCommandUtil::sortedServerList(server);
                if (servers.size() > MAX_SERVERS_TO_LIST) {
                    player.sendErrorMessage("There are too many servers set up. Use tab completion to view all servers "
                                            "available.");
                    return SINGLE_SUCCESS;
                }

                std::string list = "Available servers: ";
                for (std::size_t i = 0; i < servers.size(); i++) {
                    list += servers[i]->getServerInfo().getName();
                    if (i != servers.size() - 1) {
                        list += ", ";
                    }
                }
                player.sendMessage(list);
                return SINGLE_SUCCESS;
            })
            .then(BrigadierCommand::requiredArgumentBuilder(SERVER_ARG, StringArgumentType::word())
                      .suggests([&server](const Context &context, SuggestionsBuilder &builder) {
                          const std::string argument = context.getArguments().contains(SERVER_ARG)
                                                         ? StringArgumentType::getString(context, SERVER_ARG)
                                                         : "";
                          for (const BackendServer *sv : server.getAllServers()) {
                              const std::string &server_name = sv->getServerInfo().getName();
                              if (BuiltinCommandUtil::regionMatches(server_name, argument)) {
                                  builder.suggest(server_name);
                              }
                          }
                          return builder.build();
                      })
                      .executes([&server](const Context &context) {
                          auto &player = *context.getSource()->as<Player>();
                          const std::string server_name = StringArgumentType::getString(context, SERVER_ARG);
                          BackendServer *const to_connect = server.getServer(server_name);
                          if (to_connect == nullptr) {
                              player.sendErrorMessage(CommandMessages::serverDoesNotExist(server_name));
                              return -1;
                          }

                          player.createConnectionRequest(*to_connect).fireAndForget();
                          return SINGLE_SUCCESS;
                      }))
            .build();

    return BrigadierCommand(node);
}

}  // namespace endgate
