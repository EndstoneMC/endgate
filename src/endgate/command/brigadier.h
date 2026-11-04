#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "endgate/brigadier/arguments.h"
#include "endgate/brigadier/builder.h"
#include "endgate/brigadier/tree.h"
#include "endgate/command/source.h"

namespace endgate {

/**
 * A command that uses Brigadier for parsing the command and providing suggestions.
 *
 * @see com.velocitypowered.api.command.BrigadierCommand
 */
class BrigadierCommand {
public:
    using Source = std::shared_ptr<CommandSource>;
    using LiteralBuilder = brigadier::LiteralArgumentBuilder<Source>;
    using RequiredBuilder = brigadier::RequiredArgumentBuilder<Source>;

    /**
     * The return code used by a Command to indicate the command execution should be forwarded to the
     * backend server.
     *
     * @see com.velocitypowered.api.command.BrigadierCommand#FORWARD
     */
    static constexpr int FORWARD = static_cast<int>(0xF6287429U);

    /**
     * Constructs a BrigadierCommand from the node returned by the given builder.
     *
     * @see com.velocitypowered.api.command.BrigadierCommand#BrigadierCommand(LiteralArgumentBuilder)
     */
    explicit BrigadierCommand(const LiteralBuilder &builder) : node_(builder.build()) {}

    /**
     * Constructs a BrigadierCommand from the given command node.
     *
     * @see com.velocitypowered.api.command.BrigadierCommand#BrigadierCommand(LiteralCommandNode)
     */
    explicit BrigadierCommand(std::shared_ptr<brigadier::LiteralCommandNode<Source>> node) : node_(std::move(node))
    {
        if (!node_) {
            throw std::invalid_argument("node");
        }
    }

    /**
     * Returns the literal node for this command.
     *
     * @see com.velocitypowered.api.command.BrigadierCommand#getNode
     */
    [[nodiscard]] const std::shared_ptr<brigadier::LiteralCommandNode<Source>> &getNode() const { return node_; }

    /**
     * Creates a new LiteralArgumentBuilder of the required name.
     *
     * @see com.velocitypowered.api.command.BrigadierCommand#literalArgumentBuilder
     */
    [[nodiscard]] static LiteralBuilder literalArgumentBuilder(const std::string &name)
    {
        if (name.find(' ') != std::string::npos) {
            throw std::invalid_argument("the argument name cannot contain spaces");
        }
        return LiteralBuilder::literal(name);
    }

    /**
     * Creates a new RequiredArgumentBuilder of the required name and type.
     *
     * @see com.velocitypowered.api.command.BrigadierCommand#requiredArgumentBuilder
     */
    [[nodiscard]] static RequiredBuilder requiredArgumentBuilder(const std::string &name,
                                                                 std::shared_ptr<const brigadier::ArgumentType> type)
    {
        return RequiredBuilder::argument(name, std::move(type));
    }

private:
    std::shared_ptr<brigadier::LiteralCommandNode<Source>> node_;
};

}  // namespace endgate
