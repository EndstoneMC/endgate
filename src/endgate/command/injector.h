#pragma once

#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include <bedrock/protocol/command.h>

#include "endgate/brigadier/dispatcher.h"
#include "endgate/command/meta.h"
#include "endgate/command/source.h"

namespace endgate {

/**
 * Copies the commands of the proxy's dispatcher into the commands a backend announced, respecting the
 * requirements the given source satisfies. A Bedrock client has no command graph, so each command
 * becomes the commands, enums and soft enums of an AvailableCommandsPacket.
 *
 * @see com.velocitypowered.proxy.command.CommandGraphInjector
 */
class CommandGraphInjector {
public:
    using Source = std::shared_ptr<CommandSource>;
    using Literal = brigadier::LiteralCommandNode<Source>;
    using SoftEnums = std::map<std::string, std::vector<std::string>>;

    CommandGraphInjector(brigadier::CommandDispatcher<Source> &dispatcher, std::shared_mutex &lock,
                         const std::map<std::string, CommandMeta> &metas);

    /**
     * Adds the commands of this injector's dispatcher to the given packet, respecting the requirements
     * satisfied by the given source, and returns the soft enums it sent with their values.
     *
     * Prior to adding a command with the same name as one previously contained in the packet, the old
     * command is removed from the packet. Enums, enum values and soft enums are only ever appended to.
     *
     * @see com.velocitypowered.proxy.command.CommandGraphInjector#inject
     */
    SoftEnums inject(bedrock::protocol::AvailableCommandsPacket &dest, const Source &source, int protocol) const;

    /**
     * Returns the soft enums that inject would send to the given source, with their current values.
     */
    [[nodiscard]] SoftEnums getSoftEnums(const Source &source) const;

    /**
     * Returns why the command cannot be written as a Bedrock command that the client parses, or nothing
     * when it can.
     */
    [[nodiscard]] static std::optional<std::string> findClash(const Literal &node);

private:
    brigadier::CommandDispatcher<Source> &dispatcher_;
    std::shared_mutex &lock_;
    const std::map<std::string, CommandMeta> &metas_;
};

}  // namespace endgate
