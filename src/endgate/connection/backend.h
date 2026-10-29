#pragma once

#include <cstddef>
#include <deque>
#include <memory>

#include <spdlog/logger.h>

#include "endgate/awaitable.h"
#include "endgate/connection/connection.h"

namespace endgate {

class Player;
class RegisteredServer;

/**
 * The proxy's connection to a backend on behalf of one player. Its methods run on the player's
 * strand; the leg it opens runs on its own. What the player sends before that leg is up waits here,
 * in order, up to a bound.
 *
 * @see com.velocitypowered.api.proxy.ServerConnection
 * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection
 */
class ServerConnection : public std::enable_shared_from_this<ServerConnection> {
public:
    ServerConnection(RegisteredServer &server, std::shared_ptr<Player> player);

    /**
     * Finds out which transport the backend speaks and opens a leg to it over that transport.
     *
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#connect
     */
    void connect();

    /**
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#disconnect
     */
    void disconnect();

    void write(LegacyByteBuf batch);
    void connected();

    [[nodiscard]] RegisteredServer &getServer() const { return server_; }
    [[nodiscard]] Player &getPlayer() const { return *player_; }

    /**
     * The leg to the backend, once it is open. It never changes after that, so the leg's own strand
     * may read it too.
     *
     * @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection#getConnection
     */
    [[nodiscard]] MinecraftConnection *getConnection() const { return connection_.get(); }

private:
    static Awaitable<void> open(std::shared_ptr<ServerConnection> self);

    RegisteredServer &server_;
    std::shared_ptr<Player> player_;
    spdlog::logger &logger_;
    std::shared_ptr<MinecraftConnection> connection_;
    std::deque<LegacyByteBuf> pending_;
    std::size_t pending_bytes_{0};
    bool connected_{false};
};

/**
 * Forwards what the backend sends to the player, and takes the player down with it.
 *
 * @see com.velocitypowered.proxy.connection.backend.BackendPlaySessionHandler
 */
class BackendPlaySessionHandler final : public MinecraftSessionHandler {
public:
    explicit BackendPlaySessionHandler(std::shared_ptr<ServerConnection> server_connection);

    void connected() override;
    void handleUnknown(LegacyByteBuf batch) override;
    void disconnected() override;

private:
    spdlog::logger &logger_;
    std::shared_ptr<ServerConnection> server_connection_;
    bool connected_{false};
};

}  // namespace endgate
