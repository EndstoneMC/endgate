#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/logger.h>

#include "endgate/connection/connection.h"
#include "endgate/connection/settings.h"
#include "endgate/tablist/tablist.h"
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

    /**
     * @see com.velocitypowered.api.proxy.Player#getPlayerSettings
     */
    [[nodiscard]] const ClientSettings &getPlayerSettings() const { return settings_; }

    /**
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#setClientSettings
     */
    template <typename Packet>
    void setClientSettings(const Packet &packet)
    {
        settings_.track(packet);
    }

    /**
     * @see com.velocitypowered.api.proxy.Player#getTabList
     */
    [[nodiscard]] TabList &getTabList() { return tab_list_; }

private:
    ProxyServer &server_;
    spdlog::logger &logger_;
    std::shared_ptr<MinecraftConnection> connection_;
    GameProfile profile_;
    std::shared_ptr<ServerConnection> connected_server_;
    ClientSettings settings_;
    TabList tab_list_;
    bool active_{true};
};

/**
 * Forwards what the player sends to the backend it is on.
 *
 * @see com.velocitypowered.proxy.connection.client.ClientPlaySessionHandler
 */
class ClientPlaySessionHandler final : public HandleEach<ClientPlaySessionHandler, MinecraftSessionHandler,
                                                         RegisteredIn<StateRegistry::Play, Direction::Serverbound>> {
public:
    explicit ClientPlaySessionHandler(std::shared_ptr<Player> player);

    using HandleEach::handle;

    template <typename Packet>
    bool handleEach(const Packet &)
    {
        return false;
    }

    /**
     * @see com.velocitypowered.proxy.connection.client.ClientPlaySessionHandler#handle(ClientSettingsPacket)
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::RequestChunkRadius, bp::MinecraftPacketIds::ClientCacheStatus>
    bool handleEach(const Packet &packet)
    {
        player_->setClientSettings(packet);
        return false;
    }

    void activated() override;
    void connected() override {}
    void handleUnknown(ByteBufView batch) override;
    void readCompleted() override;
    void disconnected() override;

    /**
     * Handles the StartGame packet. This function is responsible for handling the client-side
     * switching servers in endgate. Returns whether the client takes the packet.
     *
     * @see com.velocitypowered.proxy.connection.client.ClientPlaySessionHandler#handleBackendJoinGame
     */
    [[nodiscard]] bool handleBackendStartGame(bp::ActorRuntimeID runtime_id, bp::ActorUniqueID unique_id,
                                              ServerConnection &destination);

    /**
     * @see com.velocitypowered.proxy.connection.client.ClientPlaySessionHandler#getServerBossBars
     */
    [[nodiscard]] std::vector<bp::ActorUniqueID> &getServerBossBars() { return server_boss_bars_; }

    [[nodiscard]] bp::ActorRuntimeID getRuntimeId() const { return runtime_id_; }
    [[nodiscard]] std::set<bp::ActorUniqueID> &getServerActors() { return server_actors_; }
    [[nodiscard]] std::set<std::string> &getServerObjectives() { return server_objectives_; }
    [[nodiscard]] std::set<std::int32_t> &getServerEffects() { return server_effects_; }
    [[nodiscard]] std::optional<std::pair<std::int32_t, std::int32_t>> &getServerContainer()
    {
        return server_container_;
    }

private:
    spdlog::logger &logger_;
    std::shared_ptr<Player> player_;
    bool connected_{false};
    bool spawned_{false};
    bp::ActorRuntimeID runtime_id_;
    bp::ActorUniqueID unique_id_;
    std::vector<bp::ActorUniqueID> server_boss_bars_;
    std::set<bp::ActorUniqueID> server_actors_;
    std::set<std::string> server_objectives_;
    std::set<std::int32_t> server_effects_;
    std::optional<std::pair<std::int32_t, std::int32_t>> server_container_;
};

}  // namespace endgate
