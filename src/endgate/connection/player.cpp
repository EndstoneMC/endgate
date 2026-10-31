#include "endgate/connection/player.h"

#include <exception>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <bedrock/protocol/camera.h>
#include <bedrock/protocol/sound.h>
#include <bedrock/protocol/text.h>
#include <bedrock/protocol/ui.h>

#include "endgate/config.h"
#include "endgate/connection/backend.h"
#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"
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
        player.sendMessage(ConnectionMessages::ALREADY_CONNECTED);
        break;
    case Status::ConnectionInProgress:
        player.sendMessage(ConnectionMessages::IN_PROGRESS);
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

Player::Player(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection, GameProfile profile)
    : server_(server), logger_(LogManager::getLogger(*this)), connection_(std::move(connection)),
      profile_(std::move(profile)), player_list_(*connection_)
{
}

BackendServer *Player::getNextServerToTry() const
{
    const auto &order = server_.getConfiguration().getAttemptConnectionOrder();
    if (order.empty()) {
        logger_.warn("[initial connection] {}: no available servers, try is empty", connection_->getRemoteAddress());
        return nullptr;
    }
    for (const auto &name : order) {
        if (auto *const server = server_.getServer(name)) {
            return server;
        }
    }
    logger_.warn("[initial connection] {}: no available servers, none of try is configured",
                 connection_->getRemoteAddress());
    return nullptr;
}

void Player::setConnectedServer(std::shared_ptr<ServerConnection> server_connection)
{
    connected_server_ = std::move(server_connection);
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
                  connection_->getRemoteAddress(), name, cause.what());
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
                     connection_->getRemoteAddress(), name, reason);
        handleKickEvent(server, std::format("You were kicked from {}: {}", name, reason));
        return;
    }
    logger_.error("[connected player] {} ({}): disconnected while connecting to {}: {}", profile_.name,
                  connection_->getRemoteAddress(), name, reason);
    handleKickEvent(server, std::format("Unable to connect to {}: {}", name, reason));
}

void Player::handleKickEvent(BackendServer &server, const std::string &friendly_reason)
{
    if (!connected_server_ || &connected_server_->getServer() == &server) {
        disconnect(DisconnectFailReason::NoReason, friendly_reason);
        return;
    }
    if (connection_in_flight_ && &connection_in_flight_->getServer() == &server) {
        resetInFlightConnection();
    }
    sendMessage(friendly_reason);
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
        logger_.info("[connected player] {} ({}) has disconnected", profile_.name, connection_->getRemoteAddress());
    }
    server_.unregisterConnection(*this);
    if (const auto in_flight = std::exchange(connection_in_flight_, nullptr)) {
        in_flight->disconnect();
    }
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
                      player_->getConnection().getRemoteAddress(), batch.readableBytes());
        return;
    }
    server->write(batch.copy());
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

bool ClientPlaySessionHandler::handleBackendStartGame(bp::ActorRuntimeID runtime_id, bp::ActorUniqueID unique_id,
                                                      ServerConnection &destination)
{
    if (!spawned_) {
        spawned_ = true;
        runtime_id_ = runtime_id;
        unique_id_ = unique_id;
        return true;
    }

    auto &backend = destination.ensureConnected();
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
        return true;
    });
    server_actors_.clear();
    server_boss_bars_.clear();
    server_objectives_.clear();
    server_effects_.clear();
    server_container_.reset();
    server_shapes_.clear();

    backend.setEntityMap(EntityMap{runtime_id_, unique_id_, runtime_id, unique_id});
    client.flush();
    return false;
}

}  // namespace endgate
