#include "endgate/command/manager.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include "endgate/logging/manager.h"

namespace endgate {

using brigadier::CommandContextBuilder;
using brigadier::CommandDispatcher;
using brigadier::CommandNode;
using brigadier::CommandSyntaxException;
using brigadier::ParseResults;
using brigadier::StringRange;
using brigadier::StringReader;
using brigadier::Suggestions;
using brigadier::SuggestionsBuilder;

CommandManager::CommandManager() : logger_(LogManager::getLogger(*this)) {}

CommandMeta::Builder CommandManager::metaBuilder(const std::string &alias)
{
    return CommandMeta::Builder(alias);
}

CommandMeta::Builder CommandManager::metaBuilder(const BrigadierCommand &command)
{
    return CommandMeta::Builder(command.getNode()->getName());
}

void CommandManager::registerCommand(const BrigadierCommand &command)
{
    registerCommand(metaBuilder(command).build(), command);
}

void CommandManager::registerCommand(const CommandMeta &meta, const BrigadierCommand &command)
{
    const std::shared_ptr<Literal> &literal = command.getNode();
    const std::string &primary_alias = literal->getName();
    if (const auto clash = CommandGraphInjector::findClash(*literal)) {
        logger_.error("Command '{}' is not registered: {}", primary_alias, *clash);
        return;
    }
    if (primary_alias == SuggestionsBuilder::toLowerCase(primary_alias)) {
        registerNode(literal);
    }

    for (const std::string &alias : meta.getAliases()) {
        if (primary_alias == alias) {
            continue;
        }
        registerNode(shallowCopy(*literal, alias));
    }

    const std::unique_lock lock(lock_);
    for (const std::string &alias : meta.getAliases()) {
        command_metas_.insert_or_assign(alias, meta);
    }
}

void CommandManager::unregister(const std::string &alias)
{
    const std::unique_lock lock(lock_);
    dispatcher_.getRoot()->removeChildByName(SuggestionsBuilder::toLowerCase(alias));
    command_metas_.erase(alias);
}

void CommandManager::unregister(const CommandMeta &meta)
{
    const std::unique_lock lock(lock_);
    for (const std::string &alias : meta.getAliases()) {
        const std::string lowercased = SuggestionsBuilder::toLowerCase(alias);
        const auto registered = command_metas_.find(lowercased);
        if (registered != command_metas_.end() && registered->second == meta) {
            command_metas_.erase(registered);
            dispatcher_.getRoot()->removeChildByName(lowercased);
        }
    }
}

std::optional<CommandMeta> CommandManager::getCommandMeta(const std::string &alias) const
{
    const std::shared_lock lock(lock_);
    const auto registered = command_metas_.find(alias);
    if (registered == command_metas_.end()) {
        return std::nullopt;
    }
    return registered->second;
}

bool CommandManager::executeImmediatelyAsync(const Source &source, const std::string &cmd_line)
{
    const std::string normalized_input = normalizeInput(cmd_line, true);
    std::optional<ParseResults<Source>> parsed;
    {
        const std::shared_lock lock(lock_);
        parsed.emplace(dispatcher_.parse(normalized_input, source));
    }
    return executeImmediately0(source, *parsed);
}

bool CommandManager::executeImmediately0(const Source &source, const ParseResults<Source> &parsed)
{
    try {
        return dispatcher_.execute(parsed) != BrigadierCommand::FORWARD;
    }
    catch (const CommandSyntaxException &e) {
        const bool is_syntax_error =
            &e.getType() != &CommandSyntaxException::BUILT_IN_EXCEPTIONS.dispatcherUnknownCommand();
        if (is_syntax_error) {
            source->sendMessage(e.getRawMessage());
            return true;
        }
        return false;
    }
    catch (const std::exception &e) {
        throw std::runtime_error("Unable to invoke command " + parsed.getReader().getString() + ": " + e.what());
    }
}

std::vector<std::string> CommandManager::offerSuggestions(const Source &source, const std::string &cmd_line)
{
    std::vector<std::string> texts;
    for (const brigadier::Suggestion &suggestion : offerBrigadierSuggestions(source, cmd_line).getList()) {
        texts.push_back(suggestion.getText());
    }
    return texts;
}

Suggestions CommandManager::offerBrigadierSuggestions(const Source &source, const std::string &cmd_line)
{
    const std::string normalized_input = normalizeInput(cmd_line, false);
    try {
        return provideSuggestions(normalized_input, source);
    }
    catch (const std::exception &e) {
        throw std::runtime_error("Unable to provide suggestions for " + cmd_line + ": " + e.what());
    }
}

std::vector<std::string> CommandManager::getAliases() const
{
    const std::shared_lock lock(lock_);
    std::vector<std::string> aliases;
    for (const auto &node : dispatcher_.getRoot()->getChildren()) {
        aliases.push_back(node->getName());
    }
    return aliases;
}

bool CommandManager::hasCommand(const std::string &alias) const
{
    return getCommand(alias) != nullptr;
}

bool CommandManager::hasCommand(const std::string &alias, const Source &source) const
{
    const auto command = getCommand(alias);
    return command != nullptr && command->canUse(source);
}

std::shared_ptr<CommandNode<CommandManager::Source>> CommandManager::getCommand(const std::string &alias) const
{
    const std::shared_lock lock(lock_);
    return dispatcher_.getRoot()->getChild(SuggestionsBuilder::toLowerCase(alias));
}

void CommandManager::registerNode(const std::shared_ptr<Literal> &node)
{
    const std::unique_lock lock(lock_);
    dispatcher_.getRoot()->removeChildByName(node->getName());
    dispatcher_.getRoot()->addChild(node);
}

std::string CommandManager::normalizeInput(const std::string &input, bool trim)
{
    constexpr auto whitespace = " \t\r\n\f\v";
    std::string command = input;
    if (trim) {
        const auto first = command.find_first_not_of(whitespace);
        command = first == std::string::npos ? std::string{}
                                             : command.substr(first, command.find_last_not_of(whitespace) - first + 1);
    }
    const auto first_sep = command.find(CommandDispatcher<Source>::ARGUMENT_SEPARATOR_CHAR);
    if (first_sep == std::string::npos) {
        return SuggestionsBuilder::toLowerCase(command);
    }
    return SuggestionsBuilder::toLowerCase(command.substr(0, first_sep)) + command.substr(first_sep);
}

std::shared_ptr<CommandManager::Literal> CommandManager::shallowCopy(const Literal &original,
                                                                     const std::string &new_name)
{
    auto builder = BrigadierCommand::LiteralBuilder::literal(new_name);
    builder.requires_(original.getRequirement())
        .forward(original.getRedirect(), original.getRedirectModifier(), original.isFork())
        .executes(original.getCommand());
    for (const auto &child : original.getChildren()) {
        builder.then(child);
    }
    return builder.build();
}

Suggestions CommandManager::provideSuggestions(const std::string &input, const Source &source)
{
    const std::shared_lock lock(lock_);
    StringReader reader(input);
    CommandContextBuilder<Source> context(dispatcher_, source, dispatcher_.getRoot(), 0);

    const auto first_sep = input.find(CommandDispatcher<Source>::ARGUMENT_SEPARATOR_CHAR, reader.getCursor());
    const auto alias_end = first_sep == std::string::npos ? reader.getTotalLength() : static_cast<int>(first_sep);
    const StringRange alias_range = StringRange::between(reader.getCursor(), alias_end);
    reader.setCursor(alias_range.getEnd());

    const bool has_arguments = reader.canRead();
    if (!has_arguments) {
        return provideAliasSuggestions(reader, context);
    }
    const std::string alias = SuggestionsBuilder::toLowerCase(alias_range.get(reader));
    if (!dispatcher_.getRoot()->getChild(alias)) {
        return Suggestions::empty();
    }
    return provideArgumentsSuggestions(reader, source);
}

Suggestions CommandManager::provideAliasSuggestions(const StringReader &reader,
                                                    const CommandContextBuilder<Source> &context)
{
    const Source &source = context.getSource();
    const std::string input = SuggestionsBuilder::toLowerCase(reader.getRead());

    std::vector<Suggestions> suggestions;
    for (const auto &node : context.getRootNode()->getChildren()) {
        const std::string alias = node->getName();
        if (alias.starts_with(input) && node->canUse(source)) {
            suggestions.push_back(SuggestionsBuilder(input, 0).suggest(alias).build());
        }
    }
    return Suggestions::merge(input, suggestions);
}

Suggestions CommandManager::provideArgumentsSuggestions(StringReader &reader, const Source &source)
{
    reader.setCursor(0);
    const ParseResults<Source> parse = dispatcher_.parse(reader, source);
    try {
        return dispatcher_.getCompletionSuggestions(parse);
    }
    catch (const std::exception &e) {
        logger_.error("Command node cannot provide suggestions for {}: {}", reader.getString(), e.what());
        return Suggestions::empty();
    }
}

}  // namespace endgate
