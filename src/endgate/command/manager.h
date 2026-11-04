#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include <spdlog/logger.h>

#include "endgate/brigadier/dispatcher.h"
#include "endgate/command/brigadier.h"
#include "endgate/command/injector.h"
#include "endgate/command/meta.h"
#include "endgate/command/source.h"

namespace endgate {

/**
 * Handles the registration and execution of commands.
 *
 * @see com.velocitypowered.api.command.CommandManager
 * @see com.velocitypowered.proxy.command.VelocityCommandManager
 */
class CommandManager {
public:
    using Source = std::shared_ptr<CommandSource>;

    CommandManager();

    /**
     * Returns a builder of the metadata of a command with the given alias.
     *
     * @see com.velocitypowered.api.command.CommandManager#metaBuilder(String)
     */
    [[nodiscard]] static CommandMeta::Builder metaBuilder(const std::string &alias);

    /**
     * Returns a builder of the metadata of the given command.
     *
     * @see com.velocitypowered.api.command.CommandManager#metaBuilder(BrigadierCommand)
     */
    [[nodiscard]] static CommandMeta::Builder metaBuilder(const BrigadierCommand &command);

    /**
     * Registers the specified command with the name of its node.
     *
     * @see com.velocitypowered.api.command.CommandManager#register(BrigadierCommand)
     */
    void registerCommand(const BrigadierCommand &command);

    /**
     * Registers the specified command with the specified metadata.
     *
     * @see com.velocitypowered.api.command.CommandManager#register(CommandMeta, Command)
     * @see com.velocitypowered.proxy.command.registrar.BrigadierCommandRegistrar#register
     */
    void registerCommand(const CommandMeta &meta, const BrigadierCommand &command);

    /**
     * Unregisters the command registered with the given alias.
     *
     * @see com.velocitypowered.api.command.CommandManager#unregister(String)
     */
    void unregister(const std::string &alias);

    /**
     * Unregisters every alias of the given metadata that is still registered for it.
     *
     * @see com.velocitypowered.api.command.CommandManager#unregister(CommandMeta)
     */
    void unregister(const CommandMeta &meta);

    /**
     * Returns the metadata of the command registered with the given alias.
     *
     * @see com.velocitypowered.api.command.CommandManager#getCommandMeta
     */
    [[nodiscard]] std::optional<CommandMeta> getCommandMeta(const std::string &alias) const;

    /**
     * Attempts to execute a command from the given command line, and returns whether one ran. A command
     * the source may not use is unknown to it.
     *
     * @see com.velocitypowered.api.command.CommandManager#executeImmediatelyAsync
     */
    [[nodiscard]] bool executeImmediatelyAsync(const Source &source, const std::string &cmd_line);

    /**
     * Returns suggestions to fill in the given command line.
     *
     * @see com.velocitypowered.api.command.CommandManager#offerSuggestions
     */
    [[nodiscard]] std::vector<std::string> offerSuggestions(const Source &source, const std::string &cmd_line);

    /**
     * Returns suggestions to fill in the given command line, with their ranges.
     *
     * @see com.velocitypowered.api.command.CommandManager#offerBrigadierSuggestions
     */
    [[nodiscard]] brigadier::Suggestions offerBrigadierSuggestions(const Source &source, const std::string &cmd_line);

    /**
     * Returns the aliases registered.
     *
     * @see com.velocitypowered.api.command.CommandManager#getAliases
     */
    [[nodiscard]] std::vector<std::string> getAliases() const;

    /**
     * Returns whether the given alias is registered.
     *
     * @see com.velocitypowered.api.command.CommandManager#hasCommand(String)
     */
    [[nodiscard]] bool hasCommand(const std::string &alias) const;

    /**
     * Returns whether the given alias is registered and the source may use it.
     *
     * @see com.velocitypowered.api.command.CommandManager#hasCommand(String, CommandSource)
     */
    [[nodiscard]] bool hasCommand(const std::string &alias, const Source &source) const;

    /**
     * @see com.velocitypowered.proxy.command.VelocityCommandManager#getInjector
     */
    [[nodiscard]] const CommandGraphInjector &getInjector() const { return injector_; }

private:
    using Literal = brigadier::LiteralCommandNode<Source>;

    [[nodiscard]] static std::string normalizeInput(const std::string &input, bool trim);
    [[nodiscard]] static std::shared_ptr<Literal> shallowCopy(const Literal &original, const std::string &new_name);
    [[nodiscard]] std::shared_ptr<brigadier::CommandNode<Source>> getCommand(const std::string &alias) const;
    void registerNode(const std::shared_ptr<Literal> &node);
    [[nodiscard]] bool executeImmediately0(const Source &source, const brigadier::ParseResults<Source> &parsed);

    /**
     * @see com.velocitypowered.proxy.command.SuggestionsProvider#provideSuggestions
     */
    [[nodiscard]] brigadier::Suggestions provideSuggestions(const std::string &input, const Source &source);
    [[nodiscard]] static brigadier::Suggestions provideAliasSuggestions(
        const brigadier::StringReader &reader, const brigadier::CommandContextBuilder<Source> &context);
    [[nodiscard]] brigadier::Suggestions provideArgumentsSuggestions(brigadier::StringReader &reader,
                                                                     const Source &source);

    spdlog::logger &logger_;
    mutable std::shared_mutex lock_;
    brigadier::CommandDispatcher<Source> dispatcher_;
    std::map<std::string, CommandMeta> command_metas_;
    CommandGraphInjector injector_ = CommandGraphInjector(dispatcher_, lock_, command_metas_);
};

}  // namespace endgate
