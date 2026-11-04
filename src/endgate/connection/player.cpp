#include "endgate/connection/player.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <bedrock/protocol/block.h>
#include <bedrock/protocol/camera.h>
#include <bedrock/protocol/command.h>
#include <bedrock/protocol/level.h>
#include <bedrock/protocol/movement.h>
#include <bedrock/protocol/serializer.hpp>
#include <bedrock/protocol/sound.h>
#include <bedrock/protocol/stream.hpp>
#include <bedrock/protocol/text.h>
#include <bedrock/protocol/ui.h>

#include "endgate/config.h"
#include "endgate/connection/backend.h"
#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"
#include "endgate/network/version.h"
#include "endgate/server.h"

namespace endgate {

void ConnectionRequestBuilder::fireAndForget() const
{
    asio::co_spawn(player_->getConnection().eventLoop().getExecutor(), connectWithIndication(*this), asio::detached);
}

std::optional<ConnectionRequestBuilder::Status> ConnectionRequestBuilder::checkServer(BackendServer &server) const
{
    if (player_->connection_in_flight_) {
        return Status::ConnectionInProgress;
    }
    if (player_->connected_server_ && &player_->connected_server_->getServer() == &server) {
        return Status::AlreadyConnected;
    }
    return std::nullopt;
}

Awaitable<ConnectionRequestBuilder::Result> ConnectionRequestBuilder::internalConnect(ConnectionRequestBuilder self)
{
    if (const auto status = self.checkServer(*self.to_connect_)) {
        co_return Result{*status, *self.to_connect_};
    }
    auto &player = *self.player_;
    const auto connection = std::make_shared<ServerConnection>(*self.to_connect_, self.player_);
    player.connection_in_flight_ = connection;

    const auto &active_handler = player.getConnection().getActiveSessionHandler();
    if (active_handler && active_handler->is<ClientPlaySessionHandler>()) {
        player.sendMessage(std::format("Connecting to {}…", self.to_connect_->getServerInfo().getName()));
    }

    std::optional<Result> result;
    std::exception_ptr failure;
    try {
        result.emplace(co_await connection->connect());
    }
    catch (...) {
        failure = std::current_exception();
    }
    if (player.connection_in_flight_ == connection) {
        player.resetInFlightConnection();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    co_return std::move(*result);
}

Awaitable<bool> ConnectionRequestBuilder::connectWithIndication(ConnectionRequestBuilder self)
{
    auto &player = *self.player_;
    std::optional<Result> result;
    try {
        result.emplace(co_await internalConnect(self));
    }
    catch (const std::exception &cause) {
        player.handleConnectionException(*self.to_connect_, cause);
        co_return false;
    }
    switch (result->getStatus()) {
    case Status::AlreadyConnected:
        player.sendErrorMessage(ConnectionMessages::ALREADY_CONNECTED);
        break;
    case Status::ConnectionInProgress:
        player.sendErrorMessage(ConnectionMessages::IN_PROGRESS);
        break;
    case Status::ServerDisconnected:
        player.handleConnectionException(*self.to_connect_, result->getReason().value_or(std::string{
                                                                ConnectionMessages::INTERNAL_SERVER_CONNECTION_ERROR}));
        break;
    case Status::Success:
    case Status::ConnectionCancelled:
        break;
    }
    co_return result->isSuccessful();
}

Player::Player(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection, GameProfile profile,
               std::optional<std::string> virtual_host)
    : server_(server), logger_(LogManager::getLogger(*this)), connection_(std::move(connection)),
      profile_(std::move(profile)), virtual_host_(std::move(virtual_host)), player_list_(*connection_)
{
}

const std::vector<std::string> &Player::getServersToTry()
{
    if (!servers_to_try_) {
        const auto &forced_hosts = server_.getConfiguration().getForcedHosts();
        const auto forced = forced_hosts.find(virtual_host_.value_or(""));
        servers_to_try_ = forced != forced_hosts.end() ? forced->second : std::vector<std::string>{};
    }
    if (servers_to_try_->empty()) {
        servers_to_try_ = server_.getConfiguration().getAttemptConnectionOrder();
    }
    return *servers_to_try_;
}

BackendServer *Player::getNextServerToTry(const BackendServer *current)
{
    const auto protocol = connection_->getProtocolVersion().getProtocol();
    const auto &servers_to_try = getServersToTry();
    for (auto i = try_index_; i < servers_to_try.size(); ++i) {
        auto *const to_try = server_.getServer(servers_to_try[i]);
        if ((connected_server_ && &connected_server_->getServer() == to_try) ||
            (connection_in_flight_ && &connection_in_flight_->getServer() == to_try) || (current == to_try) ||
            (to_try != nullptr && to_try->isOnOtherProtocol(protocol))) {
            continue;
        }
        try_index_ = i;
        return to_try;
    }
    return nullptr;
}

std::optional<std::string> Player::getNoServerForVersionMessage()
{
    const auto &servers_to_try = getServersToTry();
    const auto protocol = connection_->getProtocolVersion().getProtocol();
    const auto refuses = [&](const std::string &name) {
        auto *const server = server_.getServer(name);
        return server != nullptr && server->isOnOtherProtocol(protocol);
    };
    if (servers_to_try.empty() || !std::ranges::all_of(servers_to_try, refuses)) {
        return std::nullopt;
    }
    return std::format("No server here runs Minecraft {}. Try again later or contact an admin.",
                       describeProtocol(protocol));
}

void Player::setConnectedServer(std::shared_ptr<ServerConnection> server_connection)
{
    connected_server_ = std::move(server_connection);
    current_server_ = connected_server_ ? &connected_server_->getServer() : nullptr;
    try_index_ = 0;
    if (connected_server_ == connection_in_flight_) {
        connection_in_flight_ = nullptr;
    }
}

ConnectionRequestBuilder Player::createConnectionRequest(BackendServer &server)
{
    return {shared_from_this(), server};
}

void Player::handleConnectionException(BackendServer &server, const std::exception &cause)
{
    if (!isActive()) {
        return;
    }
    const auto &name = server.getServerInfo().getName();
    if (connected_server_ && &connected_server_->getServer() == &server) {
        handleKickEvent(server, std::format("Your connection to {} encountered a problem.", name));
        return;
    }
    logger_.error("[connected player] {} ({}): unable to connect to server {}: {}", profile_.name,
                  connection_->getAssociation(), name, cause.what());
    handleKickEvent(server, std::format("Unable to connect you to {}. Please try again later.", name));
}

void Player::handleConnectionException(BackendServer &server, std::string_view reason)
{
    if (!isActive()) {
        return;
    }
    const auto &name = server.getServerInfo().getName();
    if (connected_server_ && &connected_server_->getServer() == &server) {
        logger_.info("[connected player] {} ({}): kicked from server {}: {}", profile_.name,
                     connection_->getAssociation(), name, reason);
        handleKickEvent(server, std::format("You were kicked from {}: {}", name, reason));
        return;
    }
    logger_.error("[connected player] {} ({}): disconnected while connecting to {}: {}", profile_.name,
                  connection_->getAssociation(), name, reason);
    handleKickEvent(server, std::format("Unable to connect to {}: {}", name, reason));
}

void Player::handleKickEvent(BackendServer &server, const std::string &friendly_reason)
{
    const auto kicked_from_current = !connected_server_ || &connected_server_->getServer() == &server;
    auto *const redirect_to = kicked_from_current ? getNextServerToTry(&server) : nullptr;

    connection_in_flight_ = nullptr;
    const auto previous_connection = connected_server_;
    if (kicked_from_current) {
        connected_server_ = nullptr;
        current_server_ = nullptr;
    }
    if (!isActive()) {
        return;
    }
    if (redirect_to != nullptr) {
        asio::co_spawn(connection_->eventLoop().getExecutor(),
                       redirect(shared_from_this(), *redirect_to, friendly_reason), asio::detached);
        return;
    }
    if (!kicked_from_current && previous_connection) {
        sendErrorMessage(friendly_reason);
        return;
    }
    disconnect(DisconnectFailReason::NoReason,
               kicked_from_current ? getNoServerForVersionMessage().value_or(friendly_reason) : friendly_reason);
}

Awaitable<void> Player::redirect(std::shared_ptr<Player> self, BackendServer &server, std::string friendly_reason)
{
    std::optional<ConnectionRequestBuilder::Result> result;
    try {
        result.emplace(co_await self->createConnectionRequest(server).connect());
    }
    catch (const std::exception &cause) {
        self->handleConnectionException(server, cause);
        co_return;
    }
    switch (result->getStatus()) {
    case ConnectionRequestBuilder::Status::AlreadyConnected:
        self->logger_.error("[connected player] {} ({}): already connected to {}", self->profile_.name,
                            self->connection_->getAssociation(), server.getServerInfo().getName());
        break;
    case ConnectionRequestBuilder::Status::ConnectionInProgress:
    case ConnectionRequestBuilder::Status::ConnectionCancelled:
        self->disconnect(DisconnectFailReason::NoReason, result->getReason().value_or(friendly_reason));
        break;
    case ConnectionRequestBuilder::Status::ServerDisconnected:
        self->handleConnectionException(
            server, result->getReason().value_or(std::string{ConnectionMessages::INTERNAL_SERVER_CONNECTION_ERROR}));
        break;
    case ConnectionRequestBuilder::Status::Success:
        self->sendErrorMessage(friendly_reason);
        break;
    }
}

void Player::sendMessage(std::string_view message)
{
    visitProtocolVersion(connection_->getProtocolVersion().getProtocol(), [&]<int Version>() {
        using Text = PacketOf<Version, bp::MinecraftPacketIds::Text>;
        using MessageOnly = Text::MessageOnly;
        Text packet;
        packet.body = MessageOnly{.type = decltype(MessageOnly::type)::Raw, .message = std::string{message}};
        connection_->write(std::move(packet));
        return true;
    });
}

void Player::sendErrorMessage(std::string_view message)
{
    sendMessage("§c" + std::string{message});
}

void Player::updateSoftEnums()
{
    if (!active_ || soft_enums_.empty()) {
        return;
    }
    const auto current = server_.getCommandManager().getInjector().getSoftEnums(shared_from_this());
    for (auto &[name, held] : soft_enums_) {
        const auto found = current.find(name);
        const std::vector<std::string> values = found == current.end() ? std::vector<std::string>{} : found->second;
        std::vector<std::string> added;
        std::ranges::copy_if(values, std::back_inserter(added),
                             [&held](const std::string &value) { return !std::ranges::contains(held, value); });
        std::vector<std::string> removed;
        std::ranges::copy_if(held, std::back_inserter(removed),
                             [&values](const std::string &value) { return !std::ranges::contains(values, value); });
        if (added.empty() && removed.empty()) {
            continue;
        }
        bp::UpdateSoftEnumPacket packet{.enum_name = name};
        if (removed.empty()) {
            packet.values = std::move(added);
            packet.type = bp::SoftEnumUpdateType::Add;
        }
        else if (added.empty()) {
            packet.values = std::move(removed);
            packet.type = bp::SoftEnumUpdateType::Remove;
        }
        else {
            packet.values = values;
            packet.type = bp::SoftEnumUpdateType::Replace;
        }
        connection_->write(std::move(packet));
        held = values;
    }
}

void Player::disconnect()
{
    connection_->close();
}

void Player::disconnect(DisconnectFailReason reason, const std::string &message)
{
    if (connection_->eventLoop().inEventLoop()) {
        connection_->closeWith(reason, message);
        return;
    }
    connection_->eventLoop().execute(
        [connection = connection_, reason, message] { connection->closeWith(reason, message); });
}

void Player::teardown()
{
    active_ = false;
    if (server_.getConfiguration().isLogPlayerConnections()) {
        logger_.info("[connected player] {} ({}) has disconnected", profile_.name, connection_->getAssociation());
    }
    server_.unregisterConnection(*this);
    if (const auto in_flight = std::exchange(connection_in_flight_, nullptr)) {
        in_flight->disconnect();
    }
    current_server_ = nullptr;
    if (const auto server = std::exchange(connected_server_, nullptr)) {
        server->disconnect();
    }
}

void InitialConnectSessionHandler::handleUnknown(ByteBufView batch)
{
    if (const auto &server = player_->getConnectionInFlight()) {
        server->write(batch.copy());
    }
}

void InitialConnectSessionHandler::readCompleted()
{
    if (const auto &server = player_->getConnectionInFlight()) {
        server->flush();
    }
}

void InitialConnectSessionHandler::disconnected()
{
    player_->teardown();
}

ClientPlaySessionHandler::ClientPlaySessionHandler(std::shared_ptr<Player> player)
    : logger_(LogManager::getLogger(*this)), player_(std::move(player))
{
}

void ClientPlaySessionHandler::handleUnknown(ByteBufView batch)
{
    const auto &server = player_->getConnectedServer();
    if (!server) {
        logger_.debug("[initial connection] {}: dropping {} bytes, no server connection",
                      player_->getConnection().getAssociation(), batch.readableBytes());
        return;
    }
    server->write(batch.copy());
}

bool ClientPlaySessionHandler::handlePlayerCommand(const std::string &command)
{
    if (player_->getCurrentServer() == nullptr) {
        return true;
    }
    auto &server = player_->getProxyServer();
    const auto &configuration = server.getConfiguration();
    if (!server.getCommandRateLimiter().attempt(player_->getUniqueId())) {
        if (configuration.isKickOnCommandRateLimit() &&
            failed_attempts_++ >= configuration.getKickAfterRateLimitedCommands()) {
            player_->disconnect(DisconnectFailReason::NoReason, "You are sending too many commands too quickly.");
        }
        if (configuration.isForwardCommandsIfRateLimited()) {
            return false;
        }
    }
    else {
        failed_attempts_ = 0;
    }
    return handlePlayerCommandInternal(command.starts_with('/') ? command.substr(1) : command);
}

bool ClientPlaySessionHandler::handlePlayerCommandInternal(const std::string &command)
{
    auto &server = player_->getProxyServer();
    bool has_run = false;
    try {
        has_run = server.getCommandManager().executeImmediatelyAsync(player_, command);
    }
    catch (const std::exception &e) {
        logger_.info("Exception occurred while running command for {}: {}", player_->getUsername(), e.what());
        player_->sendErrorMessage("An error occurred while running this command.");
        return true;
    }
    if (server.getConfiguration().isLogCommandExecutions()) {
        logger_.info("[connected player] {} ({}) -> executed command /{}", player_->getUsername(),
                     player_->getConnection().getAssociation(), command);
    }
    return has_run;
}

void ClientPlaySessionHandler::readCompleted()
{
    if (const auto &server = player_->getConnectedServer()) {
        server->flush();
    }
}

void ClientPlaySessionHandler::disconnected()
{
    player_->teardown();
}

void ClientPlaySessionHandler::setChunkRadius(std::int32_t radius)
{
    chunk_radius_ = radius;
    pruneServerChunks();
}

void ClientPlaySessionHandler::setChunkPublisher(std::int32_t x, std::int32_t z, std::uint32_t radius)
{
    publisher_view_ = ChunkView{.x = x >> 4, .z = z >> 4, .radius = static_cast<std::int32_t>(radius >> 4) + 1};
    pruneServerChunks();
}

void ClientPlaySessionHandler::setSpawnPosition(bool respawn, std::int32_t x, std::int32_t z)
{
    constexpr std::int32_t spawn_radius = 112;

    (respawn ? respawn_view_ : world_spawn_view_) =
        ChunkView{.x = x >> 4, .z = z >> 4, .radius = (spawn_radius >> 4) + 1};
    pruneServerChunks();
}

void ClientPlaySessionHandler::setPlayerPosition(float x, float z)
{
    const std::pair chunk{static_cast<std::int32_t>(std::floor(x)) >> 4, static_cast<std::int32_t>(std::floor(z)) >> 4};
    if (player_chunk_ == chunk) {
        return;
    }
    player_chunk_ = chunk;
    pruneServerChunks();
}

void ClientPlaySessionHandler::pruneServerChunks()
{
    constexpr std::int32_t margin = 2;

    const auto requested = player_->getPlayerSettings().getViewDistance();
    if (!player_chunk_ || (!chunk_radius_ && !requested)) {
        return;
    }
    std::vector<ChunkView> views{{.x = player_chunk_->first,
                                  .z = player_chunk_->second,
                                  .radius = std::max(chunk_radius_.value_or(0) + 1, requested.value_or(0))}};
    for (const auto &view : {publisher_view_, world_spawn_view_, respawn_view_}) {
        if (view) {
            views.push_back(*view);
        }
    }
    for (auto it = server_chunks_.begin(); it != server_chunks_.end();) {
        auto held = false;
        for (const auto &view : views) {
            held = held || (std::abs(it->first - view.x) <= view.radius + margin &&
                            std::abs(it->second - view.z) <= view.radius + margin);
        }
        it = held ? std::next(it) : server_chunks_.erase(it);
    }
}

template <typename Packet>
    requires IsPacket<Packet, bp::MinecraftPacketIds::StartGame>
void ClientPlaySessionHandler::handleBackendStartGame(const Packet &start_game, ServerConnection &destination)
{
    constexpr std::int32_t nether = 1;
    constexpr std::int32_t the_end = 2;
    constexpr std::int32_t start_raining = 3001;
    constexpr std::int32_t start_thunderstorm = 3002;
    constexpr std::int32_t stop_raining = 3003;
    constexpr std::int32_t stop_thunderstorm = 3004;
    constexpr float full_intensity = 65535.0F;

    const auto dimension = static_cast<std::int32_t>(start_game.settings.spawn_settings.dimension);
    if (!spawned_) {
        spawned_ = true;
        runtime_id_ = start_game.runtime_id;
        unique_id_ = start_game.entity_id;
        dimension_ = dimension;
        setSpawnPosition(false, start_game.settings.default_spawn.x, start_game.settings.default_spawn.z);
        block_registry_checksum_ = start_game.server_block_type_registry_checksum;
        block_network_ids_are_hashes_ = start_game.block_network_ids_are_hashes;
        server_auth_block_breaking_ = start_game.movement_settings.server_auth_block_breaking;
        bp::BinaryWriter experiments{experiments_};
        bp::serialize(experiments, start_game.settings.experiments);
        auto first = start_game;
        first.server_enabled_client_side_generation = false;
        first.settings.seed = 0;
        player_->getConnection().delayedWrite(std::move(first));
        return;
    }

    auto &backend = destination.ensureConnected();
    logger_.debug("[connected player] {} ({}): clearing {} chunks", player_->getGameProfile().name,
                  player_->getConnection().getAssociation(), server_chunks_.size());
    visitProtocolVersion(backend.getProtocolVersion().getProtocol(), [&]<int Version>() {
        PacketOf<Version, bp::MinecraftPacketIds::ServerboundLoadingScreenPacket> start;
        start.loading_screen_packet_type = decltype(start.loading_screen_packet_type)::StartLoadingScreen;
        backend.write(std::move(start));
        return true;
    });
    player_->getPlayerList().clearAll();
    auto &client = player_->getConnection();
    visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
        for (const auto actor : server_actors_) {
            client.delayedWrite(PacketOf<Version, bp::MinecraftPacketIds::RemoveActor>{.entity_id = actor});
        }
        for (const auto boss : server_boss_bars_) {
            PacketOf<Version, bp::MinecraftPacketIds::BossEvent> remove;
            remove.boss_id = boss;
            remove.event_type = bp::BossEventUpdateType::Remove;
            client.delayedWrite(std::move(remove));
        }
        for (const auto &objective : server_objectives_) {
            client.delayedWrite(
                PacketOf<Version, bp::MinecraftPacketIds::RemoveObjective>{.objective_name = objective});
        }
        for (const auto effect : server_effects_) {
            using MobEffect = PacketOf<Version, bp::MinecraftPacketIds::MobEffect>;
            MobEffect remove;
            remove.runtime_id = runtime_id_;
            remove.event_id = MobEffect::Event::Remove;
            remove.effect_id = effect;
            client.delayedWrite(std::move(remove));
        }
        if (server_container_) {
            using ContainerClose = PacketOf<Version, bp::MinecraftPacketIds::ContainerClose>;
            ContainerClose close;
            close.container_id = static_cast<decltype(close.container_id)>(server_container_->first);
            close.container_type = static_cast<decltype(close.container_type)>(server_container_->second);
            close.server_initiated_close = true;
            client.delayedWrite(std::move(close));
        }
        if (!server_shapes_.empty()) {
            PacketOf<Version, bp::MinecraftPacketIds::PrimitiveShapesPacket> shapes;
            for (const auto shape : server_shapes_) {
                shapes.shapes.push_back({.network_id = shape});
            }
            client.delayedWrite(std::move(shapes));
        }
        client.delayedWrite(PacketOf<Version, bp::MinecraftPacketIds::ClientboundCloseScreen>{});
        client.delayedWrite(PacketOf<Version, bp::MinecraftPacketIds::StopSound>{.stop_all = true});

        using SetHud = PacketOf<Version, bp::MinecraftPacketIds::SetHudPacket>;
        using HudElement = decltype(SetHud::hud_element)::value_type;
        SetHud hud;
        for (auto element = 0; element < static_cast<int>(HudElement::Count); ++element) {
            hud.hud_element.push_back(static_cast<HudElement>(element));
        }
        hud.hud_visible = decltype(hud.hud_visible)::Reset;
        client.delayedWrite(std::move(hud));
        client.delayedWrite(PacketOf<Version, bp::MinecraftPacketIds::PlayerFogSetting>{});
        PacketOf<Version, bp::MinecraftPacketIds::CameraInstruction> camera;
        camera.camera_instruction.clear = true;
        camera.camera_instruction.remove_target = true;
        camera.camera_instruction.detach_from_entity = true;
        client.delayedWrite(std::move(camera));

        using SetTitle = PacketOf<Version, bp::MinecraftPacketIds::SetTitle>;
        SetTitle clear;
        clear.type = SetTitle::TitleType::Clear;
        client.delayedWrite(std::move(clear));
        SetTitle reset;
        reset.type = SetTitle::TitleType::Reset;
        client.delayedWrite(std::move(reset));

        if (dimension != dimension_) {
            PacketOf<Version, bp::MinecraftPacketIds::ChangeDimension> change;
            change.dimension_id = static_cast<decltype(change.dimension_id)>(dimension);
            change.pos = start_game.pos;
            client.delayedWrite(std::move(change));
        }
        else {
            auto sections = 24;
            if (dimension_ == nether) {
                sections = 8;
            }
            else if (dimension_ == the_end) {
                sections = 16;
            }
            std::string biomes;
            for (auto section = 0; section < sections; ++section) {
                biomes.push_back('\x01');
                biomes.push_back('\x02');
            }
            biomes.push_back('\x00');
            for (const auto &[x, z] : server_chunks_) {
                PacketOf<Version, bp::MinecraftPacketIds::FullChunkData> chunk;
                chunk.pos = {.x = x, .z = z};
                chunk.dimension_id = static_cast<decltype(chunk.dimension_id)>(dimension_);
                chunk.serialized_chunk = biomes;
                client.delayedWrite(std::move(chunk));
            }
        }

        const auto &settings = start_game.settings;
        using SetDefaultGameType = PacketOf<Version, bp::MinecraftPacketIds::SetDefaultGameType>;
        SetDefaultGameType default_game_type;
        default_game_type.default_game_type =
            static_cast<decltype(default_game_type.default_game_type)>(settings.game_type);
        client.delayedWrite(std::move(default_game_type));
        PacketOf<Version, bp::MinecraftPacketIds::SetPlayerGameType> game_type;
        game_type.player_game_type = static_cast<decltype(game_type.player_game_type)>(start_game.entity_game_type);
        client.delayedWrite(std::move(game_type));
        PacketOf<Version, bp::MinecraftPacketIds::SetDifficulty> difficulty;
        difficulty.difficulty = static_cast<decltype(difficulty.difficulty)>(settings.game_difficulty);
        client.delayedWrite(std::move(difficulty));
        PacketOf<Version, bp::MinecraftPacketIds::GameRulesChanged> rules;
        rules.rule_data = settings.rule_data;
        client.delayedWrite(std::move(rules));
        client.delayedWrite(PacketOf<Version, bp::MinecraftPacketIds::SetCommandsEnabled>{
            .commands_enabled = settings.commands_enabled});
        client.delayedWrite(PacketOf<Version, bp::MinecraftPacketIds::SetTime>{.time = settings.time});

        PacketOf<Version, bp::MinecraftPacketIds::SetSpawnPosition> spawn;
        spawn.spawn_pos_type = decltype(spawn.spawn_pos_type)::WorldSpawn;
        spawn.pos = {.x = settings.default_spawn.x, .y = settings.default_spawn.y, .z = settings.default_spawn.z};
        spawn.dimension_type = static_cast<decltype(spawn.dimension_type)>(dimension);
        spawn.spawn_block_pos = spawn.pos;
        client.delayedWrite(std::move(spawn));

        using LevelEvent = PacketOf<Version, bp::MinecraftPacketIds::LevelEvent>;
        LevelEvent rain;
        rain.event_id = settings.rain_level > 0 ? start_raining : stop_raining;
        rain.data = static_cast<std::int32_t>(settings.rain_level * full_intensity);
        client.delayedWrite(std::move(rain));
        LevelEvent thunder;
        thunder.event_id = settings.lightning_level > 0 ? start_thunderstorm : stop_thunderstorm;
        thunder.data = static_cast<std::int32_t>(settings.lightning_level * full_intensity);
        client.delayedWrite(std::move(thunder));
        return true;
    });
    server_actors_.clear();
    server_boss_bars_.clear();
    server_objectives_.clear();
    server_effects_.clear();
    server_container_.reset();
    server_shapes_.clear();
    server_chunks_.clear();
    fade_lifted_ = dimension != dimension_;
    dimension_ = dimension;
    setSpawnPosition(false, start_game.settings.default_spawn.x, start_game.settings.default_spawn.z);
    switched_ = true;
    awaiting_spawn_ = true;

    backend.setEntityMap(EntityMap{runtime_id_, unique_id_, start_game.runtime_id, start_game.entity_id});
    client.flush();
}

static_assert(std::is_same_v<PacketTypes<bp::MinecraftPacketIds::StartGame>,
                             TypeList<bp::StartGamePacket_<MINIMUM_VERSION.getProtocol()>,
                                      bp::StartGamePacket_<MAXIMUM_VERSION.getProtocol()>>>,
              "a supported version's StartGame has a type handleBackendStartGame isn't instantiated for");

template void ClientPlaySessionHandler::handleBackendStartGame(
    const bp::StartGamePacket_<MINIMUM_VERSION.getProtocol()> &start_game, ServerConnection &destination);
template void ClientPlaySessionHandler::handleBackendStartGame(
    const bp::StartGamePacket_<MAXIMUM_VERSION.getProtocol()> &start_game, ServerConnection &destination);

template <typename Packet>
    requires IsPacket<Packet, bp::MinecraftPacketIds::StartGame>
std::optional<std::string_view> ClientPlaySessionHandler::findIncompatibility(const Packet &start_game) const
{
    if (start_game.server_block_type_registry_checksum != block_registry_checksum_ ||
        start_game.block_network_ids_are_hashes != block_network_ids_are_hashes_) {
        return "it has other blocks than the server you joined";
    }
    if (start_game.movement_settings.server_auth_block_breaking != server_auth_block_breaking_) {
        return "it breaks blocks differently from the server you joined";
    }
    std::string experiments;
    bp::BinaryWriter writer{experiments};
    bp::serialize(writer, start_game.settings.experiments);
    if (experiments != experiments_) {
        return "it runs other experiments than the server you joined";
    }
    return std::nullopt;
}

template std::optional<std::string_view> ClientPlaySessionHandler::findIncompatibility(
    const bp::StartGamePacket_<MINIMUM_VERSION.getProtocol()> &start_game) const;
template std::optional<std::string_view> ClientPlaySessionHandler::findIncompatibility(
    const bp::StartGamePacket_<MAXIMUM_VERSION.getProtocol()> &start_game) const;

void ClientPlaySessionHandler::handleBackendPlayerSpawn(ServerConnection &destination)
{
    constexpr float fade_out_seconds = 0.5F;

    if (!std::exchange(awaiting_spawn_, false)) {
        return;
    }
    auto &backend = destination.ensureConnected();
    visitProtocolVersion(backend.getProtocolVersion().getProtocol(), [&]<int Version>() {
        PacketOf<Version, bp::MinecraftPacketIds::ServerboundLoadingScreenPacket> end;
        end.loading_screen_packet_type = decltype(end.loading_screen_packet_type)::EndLoadingScreen;
        backend.delayedWrite(std::move(end));
        backend.write(PacketOf<Version, bp::MinecraftPacketIds::SetLocalPlayerAsInit>{.player_id = runtime_id_});
        return true;
    });
    if (std::exchange(fade_lifted_, false)) {
        return;
    }
    auto &client = player_->getConnection();
    visitProtocolVersion(client.getProtocolVersion().getProtocol(), [&]<int Version>() {
        PacketOf<Version, bp::MinecraftPacketIds::CameraInstruction> camera;
        auto &fade = camera.camera_instruction.fade.emplace();
        fade.time = typename decltype(fade.time)::value_type{.fade_out_time = fade_out_seconds};
        fade.color.emplace();
        client.delayedWrite(std::move(camera));
        return true;
    });
}

}  // namespace endgate
