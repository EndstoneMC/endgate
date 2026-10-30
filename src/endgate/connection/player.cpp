#include "endgate/connection/player.h"

#include <utility>

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

Player::Player(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection, GameProfile profile)
    : server_(server), logger_(LogManager::getLogger(*this)), connection_(std::move(connection)),
      profile_(std::move(profile)), tab_list_(*connection_)
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

void Player::createConnectionRequest(BackendServer &server)
{
    connected_server_ = std::make_shared<ServerConnection>(server, shared_from_this());
    connected_server_->connect();
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
    server_.unregisterConnection(*this);
    if (const auto server = std::exchange(connected_server_, nullptr)) {
        server->disconnect();
    }
}

ClientPlaySessionHandler::ClientPlaySessionHandler(std::shared_ptr<Player> player)
    : logger_(LogManager::getLogger(*this)), player_(std::move(player))
{
}

void ClientPlaySessionHandler::activated()
{
    connected_ = true;
    if (player_->getConnection().getEndpoint().getServer().getConfiguration().isLogPlayerConnections()) {
        logger_.info("[connected player] {} ({}) has connected", player_->getGameProfile().name,
                     player_->getConnection().getRemoteAddress());
    }
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
    if (connected_ && player_->getConnection().getEndpoint().getServer().getConfiguration().isLogPlayerConnections()) {
        logger_.info("[connected player] {} ({}) has disconnected", player_->getGameProfile().name,
                     player_->getConnection().getRemoteAddress());
    }
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
    player_->getTabList().clearAll();
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
