#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/backend.h"
#include "endgate/command/builtin/endgate.h"
#include "endgate/command/builtin/glist.h"
#include "endgate/command/builtin/send.h"
#include "endgate/command/builtin/server.h"
#include "endgate/command/builtin/shutdown.h"
#include "endgate/connection/player.h"
#include "sources.h"
#include "support.h"

namespace endgate {
namespace {

namespace bp = bedrock::protocol;

using brigadier::SINGLE_SUCCESS;
using brigadier::StringArgumentType;
using brigadier::SuggestionsBuilder;
using Context = brigadier::CommandContext<BrigadierCommand::Source>;
using Packet = bp::AvailableCommandsPacket;
using CommandData = Packet::CommandData;

constexpr int PROTOCOL = 2225;
constexpr std::uint32_t ID = 0x100038;
constexpr std::uint32_t MESSAGE_ROOT = 0x100044;
constexpr std::uint32_t ENUM_SYMBOL = 0x300000;
constexpr std::uint32_t SOFT_ENUM_SYMBOL = 0x4100000;

bool same(const Packet::EnumData &a, const Packet::EnumData &b)
{
    return a.name == b.name && a.values == b.values;
}

bool same(const Packet::SoftEnumData &a, const Packet::SoftEnumData &b)
{
    return a.name == b.name && a.values == b.values;
}

bool same(const Packet::ChainedSubcommandData &a, const Packet::ChainedSubcommandData &b)
{
    return a.name == b.name && std::ranges::equal(a.values, b.values, [](const auto &x, const auto &y) {
               return x.sub_command_first_value == y.sub_command_first_value &&
                      x.sub_command_second_value == y.sub_command_second_value;
           });
}

bool same(const Packet::ConstrainedValueData &a, const Packet::ConstrainedValueData &b)
{
    return a.enum_value_symbol == b.enum_value_symbol && a.enum_symbol == b.enum_symbol &&
           a.constraints == b.constraints;
}

bool same(const Packet::ParamData &a, const Packet::ParamData &b)
{
    return a.name == b.name && a.parse_symbol == b.parse_symbol && a.optional == b.optional &&
           a.param_options == b.param_options;
}

bool same(const Packet::OverloadData &a, const Packet::OverloadData &b)
{
    return a.is_chaining == b.is_chaining &&
           std::ranges::equal(a.params, b.params, [](const auto &x, const auto &y) { return same(x, y); });
}

bool same(const CommandData &a, const CommandData &b)
{
    return a.name == b.name && a.description == b.description && a.flags == b.flags && a.permission == b.permission &&
           a.alias_enum == b.alias_enum && a.chained_subcommand_indexes == b.chained_subcommand_indexes &&
           std::ranges::equal(a.overloads, b.overloads, [](const auto &x, const auto &y) { return same(x, y); });
}

template <typename T>
bool sameAll(const std::vector<T> &a, const std::vector<T> &b)
{
    return std::ranges::equal(a, b, [](const T &x, const T &y) { return same(x, y); });
}

const CommandData *find(const Packet &packet, const std::string &name)
{
    const auto found = std::ranges::find(packet.commands, name, &CommandData::name);
    return found == packet.commands.end() ? nullptr : &*found;
}

std::vector<std::string> valuesOf(const Packet &packet, const Packet::EnumData &data)
{
    std::vector<std::string> values;
    values.reserve(data.values.size());
    for (const std::uint32_t index : data.values) {
        values.push_back(packet.enum_values.at(index));
    }
    return values;
}

const Packet::EnumData &enumOf(const Packet &packet, std::uint32_t symbol)
{
    return packet.enums.at(symbol & ~ENUM_SYMBOL);
}

const Packet::SoftEnumData &softEnumOf(const Packet &packet, std::uint32_t symbol)
{
    return packet.soft_enums.at(symbol & ~SOFT_ENUM_SYMBOL);
}

const Packet::EnumData *findEnum(const Packet &packet, const std::string &name)
{
    const auto found = std::ranges::find(packet.enums, name, &Packet::EnumData::name);
    return found == packet.enums.end() ? nullptr : &*found;
}

void expectConsistent(const Packet &packet)
{
    std::set<std::string> names;
    for (const CommandData &command : packet.commands) {
        EXPECT_TRUE(names.insert(command.name).second) << command.name;
        EXPECT_LT(command.alias_enum, static_cast<std::int32_t>(packet.enums.size())) << command.name;
        for (const auto &overload : command.overloads) {
            for (const auto &param : overload.params) {
                if ((param.parse_symbol & SOFT_ENUM_SYMBOL) == SOFT_ENUM_SYMBOL) {
                    EXPECT_LT(param.parse_symbol & ~SOFT_ENUM_SYMBOL, packet.soft_enums.size()) << command.name;
                }
                else if ((param.parse_symbol & ENUM_SYMBOL) == ENUM_SYMBOL) {
                    EXPECT_LT(param.parse_symbol & ~ENUM_SYMBOL, packet.enums.size()) << command.name;
                }
            }
        }
    }
    for (const auto &data : packet.enums) {
        for (const std::uint32_t index : data.values) {
            EXPECT_LT(index, packet.enum_values.size()) << data.name;
        }
    }
    std::set<std::string> values;
    for (const std::string &value : packet.enum_values) {
        EXPECT_TRUE(values.insert(value).second) << value;
    }
    std::set<std::string> soft_enums;
    for (const auto &soft_enum : packet.soft_enums) {
        EXPECT_TRUE(soft_enums.insert(soft_enum.name).second) << soft_enum.name;
    }
}

class InjectorTest : public testing::Test {
protected:
    void SetUp() override
    {
        server_.registerServer(ServerInfo::parse("lobby", "127.0.0.1:19132").value());
        server_.registerServer(ServerInfo::parse("survival", "127.0.0.1:19133").value());
        server_.registerServer(ServerInfo::parse("creative", "127.0.0.1:19134").value());

        CommandManager &commands = server_.getCommandManager();
        const auto endgate_command = EndgateCommand::create(server_);
        commands.registerCommand(CommandManager::metaBuilder(endgate_command).build(), endgate_command);
        const auto server_command = ServerCommand::create(server_);
        commands.registerCommand(CommandManager::metaBuilder(server_command).build(), server_command);
        const auto shutdown_command = ShutdownCommand::command(server_);
        commands.registerCommand(CommandManager::metaBuilder(shutdown_command).aliases({"end", "stop"}).build(),
                                 shutdown_command);
        GlistCommand(server_).registerCommand();
        SendCommand(server_).registerCommand();
    }

    [[nodiscard]] std::shared_ptr<Player> join(const std::string &name)
    {
        const auto connection =
            std::make_shared<TestConnection>(node_.endpoint(), "127.0.0.1:1", Direction::Serverbound);
        connection_ = connection;
        const auto player = std::make_shared<Player>(
            server_, connection, GameProfile{.id = UuidUtils::generateOfflinePlayerUuid(name), .name = name});
        EXPECT_TRUE(server_.registerConnection(player));
        return player;
    }

    [[nodiscard]] std::shared_ptr<CommandSource> console()
    {
        return server_.getConsoleCommandSource().shared_from_this();
    }

    [[nodiscard]] Packet injected(const std::shared_ptr<CommandSource> &source, Packet packet = {})
    {
        server_.getCommandManager().getInjector().inject(packet, source, PROTOCOL);
        return packet;
    }

    ProxyServer server_;
    std::shared_ptr<TestConnection> connection_;
    Registry registry_;
    Node node_{server_, registry_};
    std::shared_ptr<TestSource> everything_ = std::make_shared<TestSource>();
};

TEST_F(InjectorTest, APlayerGetsServerAsOneOptionalSoftEnum)
{
    const Packet packet = injected(join("Steve"));

    ASSERT_EQ(packet.commands.size(), 1U);
    const CommandData &command = packet.commands.front();
    EXPECT_EQ(command.name, "server");
    EXPECT_EQ(command.flags, 128);
    EXPECT_EQ(command.permission, bp::CommandPermissionLevel::Any);
    EXPECT_EQ(command.alias_enum, -1);
    EXPECT_TRUE(command.chained_subcommand_indexes.empty());
    ASSERT_EQ(command.overloads.size(), 1U);
    EXPECT_FALSE(command.overloads.front().is_chaining);
    ASSERT_EQ(command.overloads.front().params.size(), 1U);
    const auto &param = command.overloads.front().params.front();
    EXPECT_EQ(param.name, "server");
    EXPECT_TRUE(param.optional);
    EXPECT_EQ(param.param_options, 0);
    EXPECT_EQ(param.parse_symbol, SOFT_ENUM_SYMBOL | 0U);
    EXPECT_EQ(softEnumOf(packet, param.parse_symbol).name, "ServerNames");
    EXPECT_EQ(softEnumOf(packet, param.parse_symbol).values,
              (std::vector<std::string>{"creative", "lobby", "survival"}));
    const Packet::EnumData *names = findEnum(packet, "CommandName");
    ASSERT_NE(names, nullptr);
    EXPECT_EQ(valuesOf(packet, *names), (std::vector<std::string>{"server"}));
    expectConsistent(packet);
}

TEST_F(InjectorTest, GlistSharesTheServerNamesWithoutAll)
{
    const Packet packet = injected(everything_);

    const CommandData *command = find(packet, "glist");
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(command->overloads.size(), 1U);
    ASSERT_EQ(command->overloads.front().params.size(), 1U);
    const auto &param = command->overloads.front().params.front();
    EXPECT_TRUE(param.optional);
    EXPECT_EQ(param.parse_symbol & SOFT_ENUM_SYMBOL, SOFT_ENUM_SYMBOL);
    EXPECT_EQ(softEnumOf(packet, param.parse_symbol).name, "ServerNames");
    EXPECT_EQ(softEnumOf(packet, param.parse_symbol).values,
              (std::vector<std::string>{"creative", "lobby", "survival"}));
    EXPECT_EQ(command->flags, 128);
    expectConsistent(packet);
}

TEST_F(InjectorTest, SendTakesAPlayerAndAServerBothOptional)
{
    const Packet packet = injected(everything_);

    const CommandData *command = find(packet, "send");
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(command->overloads.size(), 1U);
    const auto &params = command->overloads.front().params;
    ASSERT_EQ(params.size(), 2U);
    EXPECT_EQ(params[0].name, "player");
    EXPECT_EQ(softEnumOf(packet, params[0].parse_symbol).name, "PlayerNames");
    EXPECT_EQ(softEnumOf(packet, params[0].parse_symbol).values, (std::vector<std::string>{"all"}));
    EXPECT_EQ(params[1].name, "server");
    EXPECT_EQ(softEnumOf(packet, params[1].parse_symbol).name, "ServerNames");
    EXPECT_TRUE(params[0].optional);
    EXPECT_TRUE(params[1].optional);
}

TEST_F(InjectorTest, EndgateHasOneEnumPerKeywordAndAnOverloadPerExecutableNode)
{
    const Packet packet = injected(everything_);

    const CommandData *command = find(packet, "endgate");
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(command->overloads.size(), 3U);
    EXPECT_TRUE(command->overloads[0].params.empty());
    for (const auto &[index, keyword, name] :
         {std::tuple{1, "info", "EndgateInfo"}, std::tuple{2, "reload", "EndgateReload"}}) {
        const auto &params = command->overloads[index].params;
        ASSERT_EQ(params.size(), 1U);
        EXPECT_EQ(params[0].name, keyword);
        EXPECT_FALSE(params[0].optional);
        EXPECT_EQ(params[0].param_options, 0);
        EXPECT_EQ(params[0].parse_symbol & ENUM_SYMBOL, ENUM_SYMBOL);
        EXPECT_EQ(enumOf(packet, params[0].parse_symbol).name, name);
        EXPECT_EQ(valuesOf(packet, enumOf(packet, params[0].parse_symbol)), (std::vector<std::string>{keyword}));
    }
}

TEST_F(InjectorTest, ShutdownKeepsItsAliasesAndTakesAMessageLast)
{
    const Packet packet = injected(console());

    const CommandData *command = find(packet, "shutdown");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(find(packet, "end"), nullptr);
    EXPECT_EQ(find(packet, "stop"), nullptr);
    ASSERT_GE(command->alias_enum, 0);
    const Packet::EnumData &aliases = packet.enums.at(static_cast<std::size_t>(command->alias_enum));
    EXPECT_EQ(aliases.name, "shutdownCommandAliases");
    EXPECT_EQ(valuesOf(packet, aliases), (std::vector<std::string>{"shutdown", "end", "stop"}));
    const Packet::EnumData *names = findEnum(packet, "CommandName");
    ASSERT_NE(names, nullptr);
    for (const char *name : {"shutdown", "end", "stop"}) {
        EXPECT_TRUE(std::ranges::contains(valuesOf(packet, *names), name)) << name;
    }
    EXPECT_EQ(command->flags, 128 | 64);
    ASSERT_EQ(command->overloads.size(), 2U);
    EXPECT_TRUE(command->overloads[0].params.empty());
    ASSERT_EQ(command->overloads[1].params.size(), 1U);
    EXPECT_EQ(command->overloads[1].params[0].name, "reason");
    EXPECT_EQ(command->overloads[1].params[0].parse_symbol, MESSAGE_ROOT);
    EXPECT_FALSE(command->overloads[1].params[0].optional);
    expectConsistent(packet);
}

TEST_F(InjectorTest, ThePlayerSeesNothingItMayNotUse)
{
    const Packet packet = injected(join("Steve"));

    for (const char *name : {"endgate", "glist", "send", "shutdown", "end", "stop"}) {
        EXPECT_EQ(find(packet, name), nullptr) << name;
    }
}

TEST_F(InjectorTest, TheSoftEnumsFollowTheServers)
{
    const auto player = join("Steve");
    const CommandGraphInjector &injector = server_.getCommandManager().getInjector();
    Packet packet;
    const auto sent = injector.inject(packet, player, PROTOCOL);

    EXPECT_EQ(sent, injector.getSoftEnums(player));
    server_.registerServer(ServerInfo::parse("minigames", "127.0.0.1:19135").value());
    EXPECT_EQ(injector.getSoftEnums(player).at("ServerNames"),
              (std::vector<std::string>{"creative", "lobby", "minigames", "survival"}));
}

TEST_F(InjectorTest, ABackendCommandOfTheSameNameIsReplacedAndNothingElseMoves)
{
    Packet backend;
    backend.enum_values = {"kick", "x"};
    backend.enums = {{.name = "KickEnum", .values = {0, 1}}};
    backend.commands = {{.name = "help"}, {.name = "server", .description = "backend"}, {.name = "kick"}};
    backend.soft_enums = {{.name = "Mine", .values = {"a"}}};

    const Packet packet = injected(join("Steve"), backend);

    ASSERT_EQ(packet.commands.size(), 3U);
    EXPECT_EQ(packet.commands[0].name, "help");
    EXPECT_EQ(packet.commands[1].name, "kick");
    EXPECT_EQ(packet.commands[2].name, "server");
    EXPECT_EQ(packet.commands[2].description, "A command of the proxy.");
    EXPECT_TRUE(std::ranges::equal(backend.enum_values, packet.enum_values | std::views::take(2)));
    EXPECT_TRUE(same(packet.enums.front(), backend.enums.front()));
    EXPECT_TRUE(same(packet.soft_enums.front(), backend.soft_enums.front()));
    expectConsistent(packet);
}

TEST_F(InjectorTest, ABackendEnumOfTheSameNameIsNeverMerged)
{
    Packet backend;
    backend.enum_values = {"tell", "w", "msg"};
    backend.enums = {{.name = "shutdownCommandAliases", .values = {0, 1, 2}}, {.name = "EndgateInfo", .values = {0}}};

    const Packet packet = injected(console(), backend);

    EXPECT_EQ(valuesOf(packet, packet.enums[0]), (std::vector<std::string>{"tell", "w", "msg"}));
    EXPECT_EQ(valuesOf(packet, packet.enums[1]), (std::vector<std::string>{"tell"}));
    const CommandData *command = find(packet, "shutdown");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(packet.enums.at(static_cast<std::size_t>(command->alias_enum)).name, "shutdownCommandAliases2");
    const CommandData *endgate = find(packet, "endgate");
    ASSERT_NE(endgate, nullptr);
    EXPECT_EQ(enumOf(packet, endgate->overloads[1].params[0].parse_symbol).name, "EndgateInfo2");
    expectConsistent(packet);
}

TEST_F(InjectorTest, ARealPacketKeepsEveryTableApartFromTheAppendedEntries)
{
    std::ifstream file(std::string(ENDGATE_COMMAND_FIXTURES) + "/available_commands_1_26_52.bin", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(bytes.empty());
    bp::BinaryReader reader{std::string_view{bytes}};
    auto decoded = bp::deserialize<Packet>(reader);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(reader.getUnreadLength(), 0U);
    const Packet &original = *decoded;

    const Packet packet = injected(console(), original);

    EXPECT_TRUE(
        std::ranges::equal(original.enum_values, packet.enum_values | std::views::take(original.enum_values.size())));
    EXPECT_EQ(packet.chained_subcommand_values, original.chained_subcommand_values);
    EXPECT_EQ(packet.postfixes, original.postfixes);
    EXPECT_TRUE(sameAll(packet.chained_subcommands, original.chained_subcommands));
    EXPECT_TRUE(sameAll(packet.constraints, original.constraints));
    ASSERT_GE(packet.enums.size(), original.enums.size());
    for (std::size_t i = 0; i < original.enums.size(); i++) {
        EXPECT_EQ(packet.enums[i].name, original.enums[i].name);
        EXPECT_TRUE(std::ranges::equal(original.enums[i].values,
                                       packet.enums[i].values | std::views::take(original.enums[i].values.size())))
            << original.enums[i].name;
    }
    ASSERT_GE(packet.soft_enums.size(), original.soft_enums.size());
    for (std::size_t i = 0; i < original.soft_enums.size(); i++) {
        EXPECT_TRUE(same(packet.soft_enums[i], original.soft_enums[i])) << original.soft_enums[i].name;
    }

    const std::set<std::string> replaced{"endgate", "glist", "send", "shutdown", "end", "stop"};
    std::vector<CommandData> kept;
    std::ranges::copy_if(original.commands, std::back_inserter(kept),
                         [&replaced](const CommandData &command) { return !replaced.contains(command.name); });
    ASSERT_GE(packet.commands.size(), kept.size());
    for (std::size_t i = 0; i < kept.size(); i++) {
        EXPECT_TRUE(same(kept[i], packet.commands[i])) << kept[i].name;
    }
    EXPECT_NE(find(packet, "shutdown"), nullptr);
    EXPECT_EQ(find(packet, "stop"), nullptr);
}

class ClashTest : public testing::Test {
protected:
    bool registers(const BrigadierCommand::LiteralBuilder &builder)
    {
        const std::string &name = builder.getLiteral();
        manager_.registerCommand(BrigadierCommand(builder));
        return manager_.hasCommand(name);
    }

    static BrigadierCommand::RequiredBuilder word(const std::string &name)
    {
        return BrigadierCommand::requiredArgumentBuilder(name, StringArgumentType::word());
    }

    static BrigadierCommand::RequiredBuilder suggesting(const std::string &name)
    {
        return word(name).suggests(
            [](const Context &, SuggestionsBuilder &builder) { return builder.suggest("alpha").build(); });
    }

    static BrigadierCommand::LiteralBuilder literal(const std::string &name)
    {
        return BrigadierCommand::literalArgumentBuilder(name);
    }

    static int run(const Context &) { return SINGLE_SUCCESS; }

    CommandManager manager_;
    std::shared_ptr<TestSource> source_ = std::make_shared<TestSource>();
};

TEST_F(ClashTest, AKeywordNextToAPlainWordIsRefused)
{
    EXPECT_FALSE(registers(literal("a").then(literal("all").executes(run)).then(word("name").executes(run))));
}

TEST_F(ClashTest, AKeywordNextToAMessageIsRefused)
{
    EXPECT_FALSE(registers(
        literal("b")
            .then(literal("all").executes(run))
            .then(
                BrigadierCommand::requiredArgumentBuilder("text", StringArgumentType::greedyString()).executes(run))));
}

TEST_F(ClashTest, TwoWordsAtOnePositionAreRefused)
{
    EXPECT_FALSE(registers(literal("c").then(word("first").executes(run)).then(suggesting("second").executes(run))));
}

TEST_F(ClashTest, AMessageThatIsNotLastIsRefused)
{
    EXPECT_FALSE(registers(
        literal("d").then(BrigadierCommand::requiredArgumentBuilder("text", StringArgumentType::greedyString())
                              .then(word("tail").executes(run)))));
}

TEST_F(ClashTest, AKeywordWithArgumentsNextToASuggestingWordIsRefused)
{
    EXPECT_FALSE(registers(
        literal("e").then(literal("all").then(word("more").executes(run))).then(suggesting("name").executes(run))));
}

TEST_F(ClashTest, TwoKeywordsThatLowercaseToOneAreRefused)
{
    EXPECT_FALSE(registers(literal("f").then(literal("On").executes(run)).then(literal("ON").executes(run))));
}

TEST_F(ClashTest, ACommandThatFitsIsRegistered)
{
    EXPECT_TRUE(
        registers(literal("g").executes(run).then(literal("on").executes(run)).then(literal("off").executes(run))));
    EXPECT_TRUE(registers(literal("h").then(literal("all").executes(run)).then(suggesting("name").executes(run))));
}

TEST_F(ClashTest, AFoldedKeywordJoinsTheSoftEnum)
{
    ASSERT_TRUE(registers(literal("warp").then(literal("home").executes(run)).then(suggesting("warp").executes(run))));

    Packet packet;
    manager_.getInjector().inject(packet, source_, PROTOCOL);

    const CommandData *command = find(packet, "warp");
    ASSERT_NE(command, nullptr);
    ASSERT_EQ(command->overloads.size(), 1U);
    ASSERT_EQ(command->overloads[0].params.size(), 1U);
    EXPECT_EQ(softEnumOf(packet, command->overloads[0].params[0].parse_symbol).name, "WarpNames");
    EXPECT_EQ(softEnumOf(packet, command->overloads[0].params[0].parse_symbol).values,
              (std::vector<std::string>{"alpha", "home"}));
}

TEST_F(ClashTest, ArgumentsOfOneNameShareASoftEnumHoldingWhatEverySharerSuggests)
{
    ASSERT_TRUE(registers(literal("visit").then(suggesting("warp").executes(run))));
    ASSERT_TRUE(registers(literal("tour").then(word("warp")
                                                   .suggests([](const Context &, SuggestionsBuilder &builder) {
                                                       return builder.suggest("alpha").suggest("all").build();
                                                   })
                                                   .executes(run))));

    Packet packet;
    const auto sent = manager_.getInjector().inject(packet, source_, PROTOCOL);

    ASSERT_EQ(packet.soft_enums.size(), 1U);
    EXPECT_EQ(packet.soft_enums[0].name, "WarpNames");
    EXPECT_EQ(packet.soft_enums[0].values, (std::vector<std::string>{"alpha"}));
    EXPECT_EQ(sent.at("WarpNames"), (std::vector<std::string>{"alpha"}));
}

TEST_F(ClashTest, WordsAndMessagesUseTheIdAndMessageRootSymbols)
{
    ASSERT_TRUE(registers(literal("say").then(word("target").then(
        BrigadierCommand::requiredArgumentBuilder("text", StringArgumentType::greedyString()).executes(run)))));
    ASSERT_TRUE(registers(literal("note").then(word("name").executes(run))));

    Packet packet;
    manager_.getInjector().inject(packet, source_, PROTOCOL);

    const CommandData *say = find(packet, "say");
    ASSERT_NE(say, nullptr);
    EXPECT_EQ(say->flags, 128 | 64);
    ASSERT_EQ(say->overloads.size(), 1U);
    ASSERT_EQ(say->overloads[0].params.size(), 2U);
    EXPECT_EQ(say->overloads[0].params[0].parse_symbol, ID);
    EXPECT_FALSE(say->overloads[0].params[0].optional);
    EXPECT_EQ(say->overloads[0].params[1].parse_symbol, MESSAGE_ROOT);
    const CommandData *note = find(packet, "note");
    ASSERT_NE(note, nullptr);
    EXPECT_EQ(note->flags, 128);
    EXPECT_FALSE(note->overloads[0].params[0].optional);
}

}  // namespace
}  // namespace endgate
