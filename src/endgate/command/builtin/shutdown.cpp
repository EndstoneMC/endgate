#include "endgate/command/builtin/shutdown.h"

#include <string>

#include "endgate/brigadier/dispatcher.h"
#include "endgate/console.h"
#include "endgate/server.h"

namespace endgate {

namespace {

using brigadier::SINGLE_SUCCESS;
using brigadier::StringArgumentType;
using Context = brigadier::CommandContext<BrigadierCommand::Source>;

}  // namespace

BrigadierCommand ShutdownCommand::command(ProxyServer &server)
{
    return BrigadierCommand(
        BrigadierCommand::literalArgumentBuilder("shutdown")
            .requires_([](const BrigadierCommand::Source &source) { return source->is<Console>(); })
            .executes([&server](const Context &) {
                server.shutdown(true);
                return SINGLE_SUCCESS;
            })
            .then(BrigadierCommand::requiredArgumentBuilder("reason", StringArgumentType::greedyString())
                      .executes([&server](const Context &context) {
                          server.shutdown(true, context.getArgument<std::string>("reason"));
                          return SINGLE_SUCCESS;
                      })));
}

}  // namespace endgate
