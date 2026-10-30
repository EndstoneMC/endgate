#pragma once

#include <cstddef>
#include <deque>
#include <memory>

#include <spdlog/logger.h>

#include "endgate/awaitable.h"
#include "endgate/connection/connection.h"
#include "endgate/connection/world.h"
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
     * What the client holds because of this backend.
     */
    [[nodiscard]] WorldState &getWorldState() { return world_state_; }

    /**
     * The leg to the backend, once it is open.
     *
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#getConnection
     */
    [[nodiscard]] MinecraftConnection *getConnection() const { return connection_.get(); }

private:
    static Awaitable<void> open(std::shared_ptr<ServerConnection> self);

    BackendServer &server_;
    std::shared_ptr<Player> player_;
    spdlog::logger &logger_;
    std::shared_ptr<MinecraftConnection> connection_;
    std::deque<ByteBuf> pending_;
    std::size_t pending_bytes_{0};
    std::shared_ptr<ScheduledFuture> connect_timeout_;
    WorldState world_state_;
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

    /**
     * Follows what the backend gives the client, and forwards the packet as it arrived. Every packet the
     * Play state registers clientbound comes through here, bar the ones with a handle of their own.
     */
    template <typename Packet>
    bool handleEach(const Packet &packet)
    {
        server_connection_->getWorldState().track(packet);
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
