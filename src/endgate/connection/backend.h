#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <spdlog/logger.h>

#include "endgate/awaitable.h"
#include "endgate/connection/connection.h"
#include "endgate/connection/player.h"
#include "endgate/network/eventloop.h"
#include "endgate/util/future.h"

namespace endgate {

class Player;
class BackendServer;
class ClientPlaySessionHandler;

/**
 * The proxy's connection to a backend on behalf of one player. Its methods run on the player's event
 * loop, and so does the leg it opens. What the player sends before that leg is up waits here, in
 * order, up to a bound.
 *
 * @see com.velocitypowered.api.proxy.ServerConnection
 * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection
 */
class ServerConnection : public std::enable_shared_from_this<ServerConnection> {
public:
    ServerConnection(BackendServer &server, std::shared_ptr<Player> player);

    /**
     * Finds out which transport the backend supports, opens a leg to it over that transport and logs
     * the player in, until the backend's StartGame completes the result.
     *
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#connect
     */
    [[nodiscard]] Awaitable<ConnectionRequestBuilder::Result> connect() { return open(shared_from_this()); }

    /**
     * Ends the connect timeout once the leg to the backend is up.
     *
     * @see io.netty.channel.ChannelOption#CONNECT_TIMEOUT_MILLIS
     */
    void connectionEstablished();

    /**
     * Disconnects from the server.
     *
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#disconnect
     */
    void disconnect();

    /**
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#isGracefulDisconnect
     */
    [[nodiscard]] bool isGracefulDisconnect() const { return graceful_disconnect_; }

    /**
     * Ensures that this server connection remains "active": the connection is established and not
     * closed, the player is still connected to the server, and the player still remains online.
     *
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#isActive
     */
    [[nodiscard]] bool isActive() const { return connection_ && !graceful_disconnect_ && player_->isActive(); }

    void write(ByteBuf batch);
    void flush();
    void connected();

    [[nodiscard]] BackendServer &getServer() const { return server_; }
    [[nodiscard]] Player &getPlayer() const { return *player_; }

    /**
     * The leg to the backend, once it is open.
     *
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#getConnection
     */
    [[nodiscard]] MinecraftConnection *getConnection() const { return connection_.get(); }

    /**
     * Ensures the connection is still active and throws an exception if it is not.
     *
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#ensureConnected
     */
    [[nodiscard]] MinecraftConnection &ensureConnected() const;

private:
    static Awaitable<ConnectionRequestBuilder::Result> open(std::shared_ptr<ServerConnection> self);

    BackendServer &server_;
    std::shared_ptr<Player> player_;
    spdlog::logger &logger_;
    std::shared_ptr<MinecraftConnection> connection_;
    std::deque<ByteBuf> pending_;
    std::size_t pending_bytes_{0};
    std::shared_ptr<ScheduledFuture> connect_timeout_;
    bool established_{false};
    bool connected_{false};
    bool graceful_disconnect_{false};
};

using ConnectionResultFuture = CompletableFuture<ConnectionRequestBuilder::Result>;

/**
 * Logs the proxy in to a backend on the player's behalf: network settings, a login self-signed with the
 * proxy's key that carries the player's client data, and the backend's encryption handshake, until the
 * backend reports the login succeeded. A Disconnect or failed PlayStatus before then completes the
 * result with the backend's reason.
 *
 * @see com.velocitypowered.proxy.connection.backend.LoginSessionHandler
 */
class LoginSessionHandler final : public HandleEach<LoginSessionHandler, MinecraftSessionHandler,
                                                    RegisteredIn<StateRegistry::Login, Direction::Clientbound>> {
public:
    LoginSessionHandler(std::shared_ptr<ServerConnection> server_connection,
                        std::shared_ptr<ConnectionResultFuture> result_future);

    using HandleEach::handle;

    void connected() override;
    bool handleEach(const bp::NetworkSettingsPacket &packet);
    bool handleEach(const bp::ServerToClientHandshakePacket &packet);
    bool handleEach(const bp::PlayStatusPacket &packet);

    /**
     * @see com.velocitypowered.proxy.connection.backend.LoginSessionHandler#handle(DisconnectPacket)
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::Disconnect>
    bool handleEach(const Packet &packet)
    {
        std::optional<std::string> reason;
        if (const auto *const messages = std::get_if<0>(&packet.messages)) {
            reason = messages->message;
        }
        result_future_->complete(
            ConnectionRequestBuilder::Result::forDisconnect(std::move(reason), server_connection_->getServer()));
        server_connection_->disconnect();
        return true;
    }

    void handleUnknown(ByteBufView packet) override;
    void disconnected() override;

private:
    spdlog::logger &logger_;
    std::shared_ptr<ServerConnection> server_connection_;
    std::shared_ptr<ConnectionResultFuture> result_future_;
    bool logged_in_{false};
};

/**
 * Forwards what the backend sends to the player, and takes the player down with it.
 *
 * @see com.velocitypowered.proxy.connection.backend.BackendPlaySessionHandler
 */
class BackendPlaySessionHandler final : public HandleEach<BackendPlaySessionHandler, MinecraftSessionHandler,
                                                          RegisteredIn<StateRegistry::Play, Direction::Clientbound>> {
public:
    explicit BackendPlaySessionHandler(std::shared_ptr<ServerConnection> server_connection);

    using HandleEach::handle;

    template <typename Packet>
    bool handleEach(const Packet &)
    {
        return false;
    }

    /**
     * @see com.velocitypowered.proxy.connection.backend.BackendPlaySessionHandler#handle(DisconnectPacket)
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::Disconnect>
    bool handleEach(const Packet &packet)
    {
        std::string reason{bp::enum_name(packet.reason)};
        if (const auto *const messages = std::get_if<0>(&packet.messages); messages && !messages->message.empty()) {
            reason = messages->message;
        }
        server_connection_->disconnect();
        server_connection_->getPlayer().handleConnectionException(server_connection_->getServer(), reason);
        return true;
    }

    /**
     * @see com.velocitypowered.proxy.connection.backend.BackendPlaySessionHandler#handle(LegacyPlayerListItemPacket)
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::PlayerList>
    bool handleEach(const Packet &packet)
    {
        server_connection_->getPlayer().getPlayerList().processLegacy(packet);
        return false;
    }

    /**
     * @see com.velocitypowered.proxy.connection.backend.BackendPlaySessionHandler#handle(BossBarPacket)
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::BossEvent>
    bool handleEach(const Packet &packet)
    {
        if (packet.event_type == bp::BossEventUpdateType::Add) {
            player_session_handler_->getServerBossBars().push_back(packet.boss_id);
        }
        else if (packet.event_type == bp::BossEventUpdateType::Remove) {
            std::erase(player_session_handler_->getServerBossBars(), packet.boss_id);
        }
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::AddActor, bp::MinecraftPacketIds::AddPainting>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->getServerActors().insert(packet.entity_id);
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::AddPlayer>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->getServerActors().insert(packet.abilities_data.target_player);
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::AddItemActor>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->getServerActors().insert(packet.id);
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::RemoveActor>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->getServerActors().erase(packet.entity_id);
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::SetDisplayObjective>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->getServerObjectives().insert(packet.objective_name);
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::RemoveObjective>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->getServerObjectives().erase(packet.objective_name);
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::MobEffect>
    bool handleEach(const Packet &packet)
    {
        if (packet.runtime_id != player_session_handler_->getRuntimeId()) {
            return false;
        }
        if (packet.event_id == Packet::Event::Remove) {
            player_session_handler_->getServerEffects().erase(packet.effect_id);
        }
        else {
            player_session_handler_->getServerEffects().insert(packet.effect_id);
        }
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::ContainerOpen>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->getServerContainer() =
            std::pair{static_cast<std::int32_t>(packet.container_id), static_cast<std::int32_t>(packet.type)};
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::PrimitiveShapesPacket>
    bool handleEach(const Packet &packet)
    {
        for (const auto &shape : packet.shapes) {
            if (shape.shape_type) {
                player_session_handler_->getServerShapes().insert(shape.network_id);
            }
            else {
                player_session_handler_->getServerShapes().erase(shape.network_id);
            }
        }
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::ContainerClose>
    bool handleEach(const Packet &)
    {
        player_session_handler_->getServerContainer().reset();
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::FullChunkData>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->getServerChunks().emplace(packet.pos.x, packet.pos.z);
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::ChunkRadiusUpdated>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->setChunkRadius(packet.chunk_radius);
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::NetworkChunkPublisherUpdate>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->setChunkPublisher(packet.position.x, packet.position.z, packet.radius);
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::SetSpawnPosition>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->setSpawnPosition(
            packet.spawn_pos_type == decltype(packet.spawn_pos_type)::PlayerRespawn, packet.pos.x, packet.pos.z);
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::ChangeDimension>
    bool handleEach(const Packet &packet)
    {
        player_session_handler_->getServerChunks().clear();
        player_session_handler_->getDimension() = static_cast<std::int32_t>(packet.dimension_id);
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::ItemRegistryPacket,
                          bp::MinecraftPacketIds::BiomeDefinitionList, bp::MinecraftPacketIds::AvailableActorIDList,
                          bp::MinecraftPacketIds::DimensionDataPacket>
    bool handleEach(const Packet &)
    {
        return player_session_handler_->hasSwitched();
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::PlayStatus>
    bool handleEach(const Packet &packet)
    {
        if (packet.status == decltype(packet.status)::PlayerSpawn) {
            player_session_handler_->handleBackendPlayerSpawn(*server_connection_);
        }
        return false;
    }

    bool beforeHandle() override;
    void connected() override {}
    void handleUnknown(ByteBufView batch) override;
    void readCompleted() override;
    void disconnected() override;

private:
    spdlog::logger &logger_;
    std::shared_ptr<ServerConnection> server_connection_;
    std::shared_ptr<ClientPlaySessionHandler> player_session_handler_;
};

/**
 * Holds a backend connection from its login until its StartGame, while the player is still on the
 * server it is leaving, or on none yet. On the first connection what the backend sends goes on to the
 * player; on a switch the player keeps what the session fixed.
 *
 * @see com.velocitypowered.proxy.connection.backend.TransitionSessionHandler
 */
class TransitionSessionHandler final : public HandleEach<TransitionSessionHandler, MinecraftSessionHandler,
                                                         RegisteredIn<StateRegistry::Play, Direction::Clientbound>> {
public:
    TransitionSessionHandler(std::shared_ptr<ServerConnection> server_connection,
                             std::shared_ptr<ConnectionResultFuture> result_future);

    using HandleEach::handle;

    template <typename Packet>
    bool handleEach(const Packet &)
    {
        return false;
    }

    /**
     * @see com.velocitypowered.proxy.connection.backend.TransitionSessionHandler#handle(JoinGamePacket)
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::StartGame>
    bool handleEach(const Packet &packet)
    {
        auto &backend = server_connection_->ensureConnected();
        auto &player = server_connection_->getPlayer();
        const auto existing_connection = player.getConnectedServer();
        if (existing_connection) {
            player.setConnectedServer(nullptr);
            existing_connection->disconnect();
        }

        auto play_handler =
            std::dynamic_pointer_cast<ClientPlaySessionHandler>(player.getConnection().getActiveSessionHandler());
        if (!play_handler) {
            play_handler = std::make_shared<ClientPlaySessionHandler>(player.shared_from_this());
            player.getConnection().setSessionHandler(play_handler);
        }
        play_handler->handleBackendStartGame(packet, *server_connection_);

        backend.setSessionHandler(std::make_shared<BackendPlaySessionHandler>(server_connection_));
        player.setConnectedServer(server_connection_);
        if (existing_connection) {
            player.getPlayerSettings().replay(backend);
        }
        result_future_->complete(ConnectionRequestBuilder::Result::successful(server_connection_->getServer()));
        return true;
    }

    /**
     * Lets the client run the pack exchange on the first connection. On a switch it answers for the
     * client when the backend offers the packs the player joined with, since Bedrock applies packs only
     * at login, and refuses the switch otherwise.
     *
     * @see com.velocitypowered.proxy.connection.backend.ConfigSessionHandler#handle(ResourcePackRequestPacket)
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::ResourcePacksInfo>
    bool handleEach(const Packet &packet)
    {
        std::vector<std::string> packs;
        for (const auto &pack : packet.resource_packs) {
            const auto &[msb, lsb] = pack.pack_id_version.id;
            packs.push_back(std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}_{}{}", msb >> 32, (msb >> 16) & 0xffff,
                                        msb & 0xffff, lsb >> 48, lsb & 0xffffffffffff,
                                        pack.pack_id_version.version.version, pack.subpack_name));
        }
        std::ranges::sort(packs);
        auto &player = server_connection_->getPlayer();
        if (!player.getConnectedServer()) {
            player.setAppliedResourcePacks(std::move(packs));
            return false;
        }
        if (packs != player.getAppliedResourcePacks()) {
            server_connection_->disconnect();
            result_future_->complete(ConnectionRequestBuilder::Result::forDisconnect(
                "it needs other resource packs than the ones you joined with", server_connection_->getServer()));
            return true;
        }
        auto &backend = server_connection_->ensureConnected();
        visitProtocolVersion(backend.getProtocolVersion().getProtocol(), [&]<int Version>() {
            using Response = PacketOf<Version, bp::MinecraftPacketIds::ResourcePackClientResponse>;
            backend.write(Response{.response = typename Response::DownloadingFinished{}});
            return true;
        });
        return true;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::ResourcePackStack>
    bool handleEach(const Packet &)
    {
        if (!server_connection_->getPlayer().getConnectedServer()) {
            return false;
        }
        auto &backend = server_connection_->ensureConnected();
        visitProtocolVersion(backend.getProtocolVersion().getProtocol(), [&]<int Version>() {
            using Response = PacketOf<Version, bp::MinecraftPacketIds::ResourcePackClientResponse>;
            backend.write(Response{.response = typename Response::ResourcePackStackFinished{}});
            return true;
        });
        return true;
    }

    /**
     * @see com.velocitypowered.proxy.connection.backend.TransitionSessionHandler#handle(DisconnectPacket)
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::Disconnect>
    bool handleEach(const Packet &packet)
    {
        std::optional<std::string> reason;
        if (const auto *const messages = std::get_if<0>(&packet.messages)) {
            reason = messages->message;
        }
        server_connection_->disconnect();
        result_future_->complete(
            ConnectionRequestBuilder::Result::forDisconnect(std::move(reason), server_connection_->getServer()));
        return true;
    }

    bool beforeHandle() override;
    void activated() override;
    void connected() override {}
    void handleUnknown(ByteBufView batch) override;
    void readCompleted() override;
    void disconnected() override;

private:
    spdlog::logger &logger_;
    std::shared_ptr<ServerConnection> server_connection_;
    std::shared_ptr<ConnectionResultFuture> result_future_;
};

}  // namespace endgate
