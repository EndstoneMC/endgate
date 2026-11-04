#include "endgate/command/injector.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace endgate {

namespace {

namespace bp = bedrock::protocol;

using brigadier::ArgumentCommandNode;
using brigadier::CommandNode;
using brigadier::StringArgumentType;
using Source = CommandGraphInjector::Source;
using Literal = CommandGraphInjector::Literal;
using Argument = ArgumentCommandNode<Source>;
using Node = CommandNode<Source>;

constexpr std::uint32_t ENUM_SYMBOL = 0x300000;
constexpr std::uint32_t SOFT_ENUM_SYMBOL = 0x4100000;
constexpr std::uint16_t NOT_CHEAT = 128;
constexpr std::uint16_t MESSAGE = 64;
constexpr auto DESCRIPTION = "A command of the proxy.";
constexpr auto COMMAND_NAME_ENUM = "CommandName";

struct HardNonTerminals {
    std::uint32_t id;
    std::uint32_t message_root;
};

const HardNonTerminals &hardNonTerminals(int protocol)
{
    static constexpr std::array<std::pair<int, HardNonTerminals>, 4> table{{
        {2168, {.id = 0x100038, .message_root = 0x100044}},
        {2169, {.id = 0x100038, .message_root = 0x100044}},
        {2193, {.id = 0x100038, .message_root = 0x100044}},
        {2225, {.id = 0x100038, .message_root = 0x100044}},
    }};
    const auto found = std::ranges::find(table, protocol, &std::pair<int, HardNonTerminals>::first);
    if (found == table.end()) {
        throw std::invalid_argument(std::format("no hard non-terminals for protocol {}", protocol));
    }
    return found->second;
}

std::string lowered(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string pascal(std::string_view text)
{
    std::string result;
    bool upper = true;
    for (const unsigned char c : text) {
        if (std::isalnum(c) == 0) {
            upper = true;
            continue;
        }
        result += static_cast<char>(upper ? std::toupper(c) : c);
        upper = false;
    }
    return result;
}

struct Parameter {
    enum class Kind : std::uint8_t {
        Keyword,
        SoftEnum,
        Word,
        Message,
    };

    Kind kind = Kind::Word;
    std::string name;
    std::string enum_name;
    std::vector<std::string> values;
    bool optionalable = false;
    bool optional = false;

    bool operator==(const Parameter &) const = default;
};

using Overload = std::vector<Parameter>;

struct Plan {
    std::vector<std::string> names;
    std::vector<Overload> overloads;
};

bool covers(const Overload &longer, const Overload &shorter)
{
    if (longer.size() < shorter.size()) {
        return false;
    }
    for (std::size_t i = 0; i < shorter.size(); i++) {
        if (longer[i] != shorter[i]) {
            return false;
        }
    }
    return std::all_of(longer.begin() + static_cast<std::ptrdiff_t>(shorter.size()), longer.end(),
                       [](const Parameter &parameter) { return parameter.optional; });
}

void dropCovered(std::vector<Overload> &overloads)
{
    std::vector<bool> covered(overloads.size());
    for (std::size_t i = 0; i < overloads.size(); i++) {
        for (std::size_t j = 0; j < overloads.size() && !covered[i]; j++) {
            if (i != j && covers(overloads[j], overloads[i])) {
                covered[i] = overloads[j].size() > overloads[i].size() || j < i;
            }
        }
    }
    std::vector<Overload> kept;
    for (std::size_t i = 0; i < overloads.size(); i++) {
        if (!covered[i]) {
            kept.push_back(std::move(overloads[i]));
        }
    }
    overloads = std::move(kept);
}

class Walker {
public:
    Walker(brigadier::CommandDispatcher<Source> &dispatcher, const Source &source, std::string command)
        : dispatcher_(dispatcher), source_(source), command_(std::move(command))
    {
    }

    void walk(const Node &node, Plan &plan)
    {
        const bool executes = static_cast<bool>(node.getCommand());
        if (executes) {
            plan.overloads.push_back(overloadOf());
        }

        std::vector<const Literal *> literals;
        const Argument *argument = nullptr;
        for (const auto &child : node.getChildren()) {
            if (!child->canUse(source_)) {
                continue;
            }
            if (const auto *literal = child->as<Literal>()) {
                literals.push_back(literal);
            }
            else if (argument == nullptr) {
                argument = child->as<Argument>();
            }
        }

        const bool folded =
            argument != nullptr && argument->getCustomSuggestions() &&
            std::ranges::all_of(literals, [](const Literal *literal) { return literal->getChildren().empty(); });
        if (argument != nullptr) {
            steps_.push_back(parameterOf(*argument, executes, folded ? literals : std::vector<const Literal *>{}));
            walk(*argument, plan);
            steps_.pop_back();
        }
        if (folded) {
            return;
        }
        for (const Literal *literal : literals) {
            const std::string keyword = lowered(literal->getLiteral());
            steps_.push_back(Parameter{.kind = Parameter::Kind::Keyword,
                                       .name = keyword,
                                       .enum_name = pascal(command_) + pascal(keyword),
                                       .values = {keyword}});
            walk(*literal, plan);
            steps_.pop_back();
        }
    }

private:
    [[nodiscard]] Overload overloadOf() const
    {
        Overload overload = steps_;
        for (auto parameter = overload.rbegin(); parameter != overload.rend() && parameter->optionalable; ++parameter) {
            parameter->optional = true;
        }
        return overload;
    }

    [[nodiscard]] Parameter parameterOf(const Argument &argument, bool parent_executes,
                                        const std::vector<const Literal *> &folded) const
    {
        const auto *type = argument.getType()->as<StringArgumentType>();
        Parameter parameter{.name = argument.getName()};
        if (argument.getCustomSuggestions()) {
            parameter.kind = Parameter::Kind::SoftEnum;
            parameter.enum_name = softEnumName(argument.getName());
            parameter.values = suggestionsOf(argument);
            for (const Literal *literal : folded) {
                const std::string keyword = lowered(literal->getLiteral());
                if (!std::ranges::contains(parameter.values, keyword)) {
                    parameter.values.push_back(keyword);
                }
            }
            parameter.optionalable = parent_executes;
        }
        else if (type != nullptr && type->getType() == StringArgumentType::StringType::GreedyPhrase) {
            parameter.kind = Parameter::Kind::Message;
        }
        else {
            parameter.optionalable = parent_executes;
        }
        return parameter;
    }

    [[nodiscard]] static std::string softEnumName(const std::string &argument)
    {
        std::string name = pascal(argument);
        if (lowered(name).ends_with("names")) {
            return name;
        }
        return lowered(name).ends_with("name") ? name + "s" : name + "Names";
    }

    [[nodiscard]] std::vector<std::string> suggestionsOf(const Argument &argument) const
    {
        const auto context =
            brigadier::CommandContextBuilder<Source>(dispatcher_, source_, dispatcher_.getRoot(), 0).build("");
        brigadier::SuggestionsBuilder builder("", 0);
        std::vector<std::string> values;
        for (const brigadier::Suggestion &suggestion : argument.listSuggestions(context, builder).getList()) {
            values.push_back(suggestion.getText());
        }
        return values;
    }

    brigadier::CommandDispatcher<Source> &dispatcher_;
    const Source &source_;
    std::string command_;
    std::vector<Parameter> steps_;
};

std::string clashIn(const Node &node, const std::string &path)
{
    if (node.getRedirect() != nullptr) {
        return std::format("'{}' redirects, which has no counterpart", path);
    }
    std::vector<const Literal *> literals;
    std::vector<const Argument *> arguments;
    for (const auto &child : node.getChildren()) {
        if (const auto *literal = child->as<Literal>()) {
            literals.push_back(literal);
        }
        else if (const auto *argument = child->as<Argument>()) {
            arguments.push_back(argument);
        }
    }

    std::set<std::string> keywords;
    for (const Literal *literal : literals) {
        if (!keywords.insert(lowered(literal->getLiteral())).second) {
            return std::format("'{}' has the keyword '{}' twice once lowercased", path, lowered(literal->getLiteral()));
        }
    }
    if (arguments.size() > 1) {
        return std::format("'{}' has two Id-class parameters at one position: '{}' and '{}'", path,
                           arguments[0]->getName(), arguments[1]->getName());
    }
    if (arguments.empty()) {
        for (const Literal *literal : literals) {
            if (auto clash = clashIn(*literal, path + " " + literal->getLiteral()); !clash.empty()) {
                return clash;
            }
        }
        return {};
    }

    const Argument &argument = *arguments.front();
    const std::string argument_path = path + " <" + argument.getName() + ">";
    const auto *type = argument.getType()->as<StringArgumentType>();
    if (type == nullptr) {
        return std::format("'{}' takes {}, which has no counterpart", argument_path, argument.getType()->toString());
    }
    const bool greedy = type->getType() == StringArgumentType::StringType::GreedyPhrase;
    if (greedy && !argument.getChildren().empty()) {
        return std::format("'{}' is a message and is not last", argument_path);
    }
    if (!literals.empty() && (greedy || !argument.getCustomSuggestions())) {
        return std::format("'{}' has the keyword '{}' next to the Id-class parameter '{}'; the parser takes every "
                           "word as the parameter, so make it a suggesting argument or drop the keyword",
                           path, literals.front()->getLiteral(), argument.getName());
    }
    for (const Literal *literal : literals) {
        if (!literal->getChildren().empty()) {
            return std::format("'{}' has the keyword '{}' with arguments of its own next to the suggesting "
                               "parameter '{}'; the parser takes every word as the parameter",
                               path, literal->getLiteral(), argument.getName());
        }
    }
    return clashIn(argument, argument_path);
}

class PacketEditor {
public:
    explicit PacketEditor(bp::AvailableCommandsPacket &packet) : packet_(packet)
    {
        for (std::size_t i = 0; i < packet_.enum_values.size(); i++) {
            values_.try_emplace(packet_.enum_values[i], static_cast<std::uint32_t>(i));
        }
        for (std::size_t i = 0; i < packet_.enums.size(); i++) {
            enums_.try_emplace(packet_.enums[i].name, static_cast<std::uint32_t>(i));
        }
        for (std::size_t i = 0; i < packet_.soft_enums.size(); i++) {
            soft_enums_.try_emplace(packet_.soft_enums[i].name, static_cast<std::uint32_t>(i));
        }
    }

    std::uint32_t enumValue(const std::string &text)
    {
        const auto [found, inserted] =
            values_.try_emplace(text, static_cast<std::uint32_t>(packet_.enum_values.size()));
        if (inserted) {
            packet_.enum_values.push_back(text);
        }
        return found->second;
    }

    std::uint32_t addEnum(const std::string &name, const std::vector<std::string> &values)
    {
        for (int attempt = 1;; attempt++) {
            const std::string candidate = attempt == 1 ? name : name + std::to_string(attempt);
            const auto found = enums_.find(candidate);
            if (found == enums_.end()) {
                const auto index = static_cast<std::uint32_t>(packet_.enums.size());
                bp::AvailableCommandsPacket::EnumData data{.name = candidate};
                for (const std::string &value : values) {
                    data.values.push_back(enumValue(value));
                }
                packet_.enums.push_back(std::move(data));
                enums_.emplace(candidate, index);
                return index;
            }
            if (valuesOf(packet_.enums[found->second]) == values) {
                return found->second;
            }
        }
    }

    void appendToEnum(const std::string &name, const std::vector<std::string> &values)
    {
        std::uint32_t index = 0;
        if (const auto found = enums_.find(name); found != enums_.end()) {
            index = found->second;
        }
        else {
            index = addEnum(name, {});
        }
        for (const std::string &value : values) {
            const std::uint32_t value_index = enumValue(value);
            auto &indices = packet_.enums[index].values;
            if (!std::ranges::contains(indices, value_index)) {
                indices.push_back(value_index);
            }
        }
    }

    std::uint32_t addSoftEnum(const std::string &name, const std::vector<std::string> &values)
    {
        const auto found = soft_enums_.find(name);
        if (found == soft_enums_.end()) {
            const auto index = static_cast<std::uint32_t>(packet_.soft_enums.size());
            packet_.soft_enums.push_back({.name = name, .values = values});
            soft_enums_.emplace(name, index);
            return index;
        }
        auto &existing = packet_.soft_enums[found->second].values;
        for (const std::string &value : values) {
            if (!std::ranges::contains(existing, value)) {
                existing.push_back(value);
            }
        }
        return found->second;
    }

    void removeCommands(const std::vector<std::string> &names)
    {
        std::erase_if(packet_.commands, [&names](const bp::AvailableCommandsPacket::CommandData &command) {
            return std::ranges::contains(names, command.name);
        });
    }

    void addCommand(bp::AvailableCommandsPacket::CommandData command)
    {
        packet_.commands.push_back(std::move(command));
    }

private:
    [[nodiscard]] std::vector<std::string> valuesOf(const bp::AvailableCommandsPacket::EnumData &data) const
    {
        std::vector<std::string> values;
        for (const std::uint32_t index : data.values) {
            if (index < packet_.enum_values.size()) {
                values.push_back(packet_.enum_values[index]);
            }
        }
        return values;
    }

    bp::AvailableCommandsPacket &packet_;
    std::unordered_map<std::string, std::uint32_t> values_;
    std::unordered_map<std::string, std::uint32_t> enums_;
    std::unordered_map<std::string, std::uint32_t> soft_enums_;
};

void shareSoftEnums(std::vector<Plan> &plans)
{
    std::map<std::string, std::vector<std::string>> shared;
    for (const Plan &plan : plans) {
        for (const Overload &overload : plan.overloads) {
            for (const Parameter &parameter : overload) {
                if (parameter.kind != Parameter::Kind::SoftEnum) {
                    continue;
                }
                auto [found, inserted] = shared.try_emplace(parameter.enum_name, parameter.values);
                if (!inserted) {
                    std::erase_if(found->second, [&parameter](const std::string &value) {
                        return !std::ranges::contains(parameter.values, value);
                    });
                }
            }
        }
    }
    for (Plan &plan : plans) {
        for (Overload &overload : plan.overloads) {
            for (Parameter &parameter : overload) {
                if (parameter.kind == Parameter::Kind::SoftEnum) {
                    parameter.values = shared.at(parameter.enum_name);
                }
            }
        }
    }
}

std::vector<Plan> plansOf(brigadier::CommandDispatcher<Source> &dispatcher, std::shared_mutex &lock,
                          const std::map<std::string, CommandMeta> &metas, const Source &source)
{
    const std::shared_lock guard(lock);
    std::vector<Plan> plans;
    std::set<std::string> done;
    const auto root = dispatcher.getRoot();
    for (const auto &child : root->getChildren()) {
        if (!child->canUse(source) || !child->is<Literal>()) {
            continue;
        }
        std::vector<std::string> names{child->getName()};
        if (const auto meta = metas.find(child->getName()); meta != metas.end()) {
            names.clear();
            for (const std::string &alias : meta->second.getAliases()) {
                const auto registered = metas.find(alias);
                if (root->getChild(alias) != nullptr && registered != metas.end() &&
                    registered->second == meta->second) {
                    names.push_back(alias);
                }
            }
        }
        if (!done.insert(names.front()).second) {
            continue;
        }
        Plan plan{.names = std::move(names)};
        Walker(dispatcher, source, plan.names.front()).walk(*child, plan);
        dropCovered(plan.overloads);
        if (!plan.overloads.empty()) {
            plans.push_back(std::move(plan));
        }
    }
    shareSoftEnums(plans);
    return plans;
}

}  // namespace

CommandGraphInjector::CommandGraphInjector(brigadier::CommandDispatcher<Source> &dispatcher, std::shared_mutex &lock,
                                           const std::map<std::string, CommandMeta> &metas)
    : dispatcher_(dispatcher), lock_(lock), metas_(metas)
{
}

CommandGraphInjector::SoftEnums CommandGraphInjector::inject(bp::AvailableCommandsPacket &dest, const Source &source,
                                                             int protocol) const
{
    const HardNonTerminals &symbols = hardNonTerminals(protocol);
    SoftEnums soft_enums;
    PacketEditor packet(dest);
    for (const Plan &plan : plansOf(dispatcher_, lock_, metas_, source)) {
        packet.removeCommands(plan.names);
        packet.appendToEnum(COMMAND_NAME_ENUM, plan.names);

        bp::AvailableCommandsPacket::CommandData command{.name = plan.names.front(),
                                                         .description = DESCRIPTION,
                                                         .flags = NOT_CHEAT,
                                                         .permission = bp::CommandPermissionLevel::Any,
                                                         .alias_enum = -1};
        if (plan.names.size() > 1) {
            command.alias_enum =
                static_cast<std::int32_t>(packet.addEnum(plan.names.front() + "CommandAliases", plan.names));
        }
        for (const Overload &overload : plan.overloads) {
            bp::AvailableCommandsPacket::OverloadData data;
            for (const Parameter &parameter : overload) {
                bp::AvailableCommandsPacket::ParamData param{.name = parameter.name, .optional = parameter.optional};
                switch (parameter.kind) {
                case Parameter::Kind::Keyword:
                    param.parse_symbol = ENUM_SYMBOL | packet.addEnum(parameter.enum_name, parameter.values);
                    break;
                case Parameter::Kind::SoftEnum:
                    param.parse_symbol = SOFT_ENUM_SYMBOL | packet.addSoftEnum(parameter.enum_name, parameter.values);
                    soft_enums[parameter.enum_name] = parameter.values;
                    break;
                case Parameter::Kind::Word:
                    param.parse_symbol = symbols.id;
                    break;
                case Parameter::Kind::Message:
                    param.parse_symbol = symbols.message_root;
                    command.flags = static_cast<std::uint16_t>(command.flags | MESSAGE);
                    break;
                }
                data.params.push_back(std::move(param));
            }
            command.overloads.push_back(std::move(data));
        }
        packet.addCommand(std::move(command));
    }
    return soft_enums;
}

CommandGraphInjector::SoftEnums CommandGraphInjector::getSoftEnums(const Source &source) const
{
    SoftEnums soft_enums;
    for (const Plan &plan : plansOf(dispatcher_, lock_, metas_, source)) {
        for (const Overload &overload : plan.overloads) {
            for (const Parameter &parameter : overload) {
                if (parameter.kind == Parameter::Kind::SoftEnum) {
                    soft_enums[parameter.enum_name] = parameter.values;
                }
            }
        }
    }
    return soft_enums;
}

std::optional<std::string> CommandGraphInjector::findClash(const Literal &node)
{
    std::string clash = clashIn(node, node.getName());
    if (clash.empty()) {
        return std::nullopt;
    }
    return clash;
}

}  // namespace endgate
