#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include <spdlog/logger.h>

#include "endgate/awaitable.h"
#include "endgate/connection/connection.h"
#include "endgate/connection/player.h"
#include "endgate/network/eventloop.h"

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
     * Finds out which transport the backend supports and opens a leg to it over that transport.
     *
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#connect
     */
    void connect();

    /**
     * Ends the connect timeout once the leg to the backend is up.
     *
     * @see io.netty.channel.ChannelOption#CONNECT_TIMEOUT_MILLIS
     */
    void connectionEstablished();

    /**
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#disconnect
     */
    void disconnect();

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
     * Ensures that this server connection remains "active": the connection is established and not
     * closed, the player is still connected to the server, and the player still remains online.
     *
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#ensureConnected
     */
    [[nodiscard]] MinecraftConnection &ensureConnected() const;

private:
    static Awaitable<void> open(std::shared_ptr<ServerConnection> self);

    BackendServer &server_;
    std::shared_ptr<Player> player_;
    spdlog::logger &logger_;
    std::shared_ptr<MinecraftConnection> connection_;
    std::deque<ByteBuf> pending_;
    std::size_t pending_bytes_{0};
    std::shared_ptr<ScheduledFuture> connect_timeout_;
    bool established_{false};
    bool connected_{false};
};

/**
 * Logs the proxy in to a backend on the player's behalf: network settings, a login self-signed with the
 * proxy's key that carries the player's client data, and the backend's encryption handshake, until the
 * backend reports the login succeeded. A Disconnect or failed PlayStatus before then goes on to the
 * player.
 *
 * @see com.velocitypowered.proxy.connection.backend.LoginSessionHandler
 */
class LoginSessionHandler final : public MinecraftSessionHandler {
public:
    explicit LoginSessionHandler(std::shared_ptr<ServerConnection> server_connection);

    using MinecraftSessionHandler::handle;

    void connected() override;
    bool handle(const bp::NetworkSettingsPacket &packet) override;
    bool handle(const bp::ServerToClientHandshakePacket &packet) override;
    bool handle(const bp::PlayStatusPacket &packet) override;
    void handleGeneric(ByteBufView packet) override;
    void handleUnknown(ByteBufView packet) override;
    void disconnected() override;

private:
    spdlog::logger &logger_;
    std::shared_ptr<ServerConnection> server_connection_;
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
     * @see com.velocitypowered.proxy.connection.backend.BackendPlaySessionHandler#handle(LegacyPlayerListItemPacket)
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::PlayerList>
    bool handleEach(const Packet &packet)
    {
        server_connection_->getPlayer().getTabList().processLegacy(packet);
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
        requires IsPacket<Packet, bp::MinecraftPacketIds::ContainerClose>
    bool handleEach(const Packet &)
    {
        player_session_handler_->getServerContainer().reset();
        return false;
    }

    void activated() override;
    void connected() override {}
    bool handle(const bp::StartGamePacket_<MINIMUM_VERSION.getProtocol()> &packet) override;
    bool handle(const bp::StartGamePacket_<MAXIMUM_VERSION.getProtocol()> &packet) override;
    void handleUnknown(ByteBufView batch) override;
    void readCompleted() override;
    void disconnected() override;

private:
    spdlog::logger &logger_;
    std::shared_ptr<ServerConnection> server_connection_;
    std::shared_ptr<ClientPlaySessionHandler> player_session_handler_;
    bool connected_{false};
};

}  // namespace endgate
