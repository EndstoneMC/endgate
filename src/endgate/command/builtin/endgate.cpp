#include "endgate/command/builtin/endgate.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "endgate/brigadier/dispatcher.h"
#include "endgate/server.h"

namespace endgate {

namespace {

using brigadier::SINGLE_SUCCESS;
using Context = brigadier::CommandContext<BrigadierCommand::Source>;

}  // namespace

BrigadierCommand EndgateCommand::create(ProxyServer &server)
{
    const auto info =
        BrigadierCommand::literalArgumentBuilder("info")
            .requires_([](const BrigadierCommand::Source &source) {
                return source->getPermissionValue("endgate.command.info") == Tristate::True;
            })
            .executes([](const Context &context) {
                const ProxyVersion version = ProxyServer::getVersion();
                const auto year = static_cast<int>(
                    std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())}
                        .year());
                const BrigadierCommand::Source &source = context.getSource();
                source->sendMessage(std::format("§3§l{} §r§3{}", version.getName(), version.getVersion()));
                source->sendMessage(std::format("Copyright 2026-{} {}. {} is licensed under the terms of the GNU "
                                                "Affero General Public License v3.",
                                                year, version.getVendor(), version.getName()));
                if (version.getName() == "Endgate") {
                    source->sendMessage("§ahttps://endstone.dev§r - §ahttps://github.com/EndstoneMC/endgate");
                }
                return SINGLE_SUCCESS;
            })
            .build();
    const auto reload = BrigadierCommand::literalArgumentBuilder("reload")
                            .requires_([](const BrigadierCommand::Source &source) {
                                return source->getPermissionValue("endgate.command.reload") == Tristate::True;
                            })
                            .executes([&server](const Context &context) {
                                CommandSource &source = *context.getSource();
                                try {
                                    if (server.reloadConfiguration()) {
                                        source.sendMessage("§aEndgate configuration successfully reloaded.");
                                    }
                                    else {
                                        source.sendMessage("§cUnable to reload your Endgate configuration. Check the "
                                                           "console for more details.");
                                    }
                                }
                                catch (const std::exception &) {
                                    source.sendMessage("§cUnable to reload your Endgate configuration. Check the "
                                                       "console for more details.");
                                }
                                return SINGLE_SUCCESS;
                            })
                            .build();

    const std::vector<std::shared_ptr<brigadier::LiteralCommandNode<BrigadierCommand::Source>>> commands{info, reload};
    auto root = BrigadierCommand::literalArgumentBuilder("endgate");
    root.executes([commands](const Context &context) {
            const BrigadierCommand::Source &source = context.getSource();
            std::string available_commands;
            for (const auto &command : commands) {
                if (!command->getRequirement()(source)) {
                    continue;
                }
                if (!available_commands.empty()) {
                    available_commands += "|";
                }
                available_commands += command->getName();
            }
            source->sendMessage(std::format("§c/endgate <{}>", available_commands));
            return SINGLE_SUCCESS;
        })
        .requires_([commands](const BrigadierCommand::Source &source) {
            return std::ranges::any_of(commands,
                                       [&source](const auto &command) { return command->getRequirement()(source); });
        });
    for (const auto &command : commands) {
        root.then(command);
    }
    return BrigadierCommand(root);
}

}  // namespace endgate
