#pragma once

#include <memory>
#include <string>

#include <spdlog/logger.h>

#include "endgate/connection/connection.h"
#include "endgate/util/uuid.h"

namespace endgate {

class ProxyServer;

/**
 * @see com.velocitypowered.api.util.GameProfile
 */
struct GameProfile {
    UUID id;
    std::string xuid;
    std::string name;
    std::string client_data;
};
class BackendServer;
class ServerConnection;

/**
 * A player on the proxy, and the backend it is on. Everything runs on the player's event loop.
 *
 * @see com.velocitypowered.api.proxy.Player
 * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer
 */
class Player : public std::enable_shared_from_this<Player> {
public:
    Player(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection, GameProfile profile = {});

    /**
     * @see com.velocitypowered.api.proxy.Player#getGameProfile
     */
    [[nodiscard]] const GameProfile &getGameProfile() const { return profile_; }

    /**
     * @see com.velocitypowered.api.proxy.Player#getUsername
     */
    [[nodiscard]] const std::string &getUsername() const { return profile_.name; }

    /**
     * @see com.velocitypowered.api.proxy.Player#getUniqueId
     */
    [[nodiscard]] const UUID &getUniqueId() const { return profile_.id; }

    [[nodiscard]] MinecraftConnection &getConnection() const { return *connection_; }

    /**
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#getConnectedServer
     */
    [[nodiscard]] const std::shared_ptr<ServerConnection> &getConnectedServer() const { return connected_server_; }

    /**
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#getNextServerToTry
     */
    [[nodiscard]] BackendServer *getNextServerToTry() const;

    /**
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#createConnectionRequest
     */
    void createConnectionRequest(BackendServer &server);

    /**
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#disconnect
     */
    void disconnect();

    /**
     * Kicks the player with the reason, from any thread.
     *
     * @see com.velocitypowered.api.proxy.Player#disconnect
     */
    void disconnect(DisconnectFailReason reason, const std::string &message = {});

    /**
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#teardown
     */
    void teardown();

    /**
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#isActive
     */
    [[nodiscard]] bool isActive() const { return active_; }

private:
    ProxyServer &server_;
    spdlog::logger &logger_;
    std::shared_ptr<MinecraftConnection> connection_;
    GameProfile profile_;
    std::shared_ptr<ServerConnection> connected_server_;
    bool active_{true};
};

/**
 * Forwards what the player sends to the backend it is on.
 *
 * @see com.velocitypowered.proxy.connection.client.ClientPlaySessionHandler
 */
class ClientPlaySessionHandler final : public MinecraftSessionHandler {
public:
    explicit ClientPlaySessionHandler(std::shared_ptr<Player> player);

    void activated() override;
    void connected() override {}
    void handleUnknown(ByteBufView batch) override;
    void readCompleted() override;
    void disconnected() override;

private:
    spdlog::logger &logger_;
    std::shared_ptr<Player> player_;
    bool connected_{false};
};

}  // namespace endgate
