#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/logger.h>

#include "endgate/awaitable.h"
#include "endgate/command/source.h"
#include "endgate/connection/connection.h"
#include "endgate/connection/settings.h"
#include "endgate/playerlist/playerlist.h"
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
class Player;
class ServerConnection;

/**
 * @see com.velocitypowered.proxy.connection.util.ConnectionMessages
 */
struct ConnectionMessages {
    static constexpr std::string_view ALREADY_CONNECTED = "You are already connected to this server!";
    static constexpr std::string_view IN_PROGRESS = "You are already trying to connect to a server!";
    static constexpr std::string_view INTERNAL_SERVER_CONNECTION_ERROR =
        "An internal server connection error occurred.";
};

/**
 * Connects a player to a server.
 *
 * @see com.velocitypowered.api.proxy.ConnectionRequestBuilder
 * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer.ConnectionRequestBuilderImpl
 */
class ConnectionRequestBuilder {
public:
    /**
     * Represents the status of a connection request initiated by a ConnectionRequestBuilder.
     *
     * @see com.velocitypowered.api.proxy.ConnectionRequestBuilder.Status
     */
    enum class Status : std::uint8_t {
        Success,
        AlreadyConnected,
        ConnectionInProgress,
        ConnectionCancelled,
        ServerDisconnected,
    };

    /**
     * Represents the result of a connection request.
     *
     * @see com.velocitypowered.api.proxy.ConnectionRequestBuilder.Result
     * @see com.velocitypowered.proxy.connection.util.ConnectionRequestResults.Impl
     */
    class Result {
    public:
        Result(Status status, BackendServer &attempted_connection, std::optional<std::string> reason = std::nullopt)
            : status_(status), reason_(std::move(reason)), attempted_connection_(&attempted_connection)
        {
        }

        /**
         * @see com.velocitypowered.proxy.connection.util.ConnectionRequestResults#successful
         */
        static Result successful(BackendServer &server) { return {Status::Success, server}; }

        /**
         * @see com.velocitypowered.proxy.connection.util.ConnectionRequestResults#forDisconnect
         */
        static Result forDisconnect(std::optional<std::string> reason, BackendServer &server)
        {
            return {Status::ServerDisconnected, server, std::move(reason)};
        }

        /**
         * @see com.velocitypowered.api.proxy.ConnectionRequestBuilder.Result#isSuccessful
         */
        [[nodiscard]] bool isSuccessful() const { return status_ == Status::Success; }

        /**
         * @see com.velocitypowered.api.proxy.ConnectionRequestBuilder.Result#getStatus
         */
        [[nodiscard]] Status getStatus() const { return status_; }

        /**
         * @see com.velocitypowered.api.proxy.ConnectionRequestBuilder.Result#getReasonComponent
         */
        [[nodiscard]] const std::optional<std::string> &getReason() const { return reason_; }

        /**
         * @see com.velocitypowered.api.proxy.ConnectionRequestBuilder.Result#getAttemptedConnection
         */
        [[nodiscard]] BackendServer &getAttemptedConnection() const { return *attempted_connection_; }

    private:
        Status status_;
        std::optional<std::string> reason_;
        BackendServer *attempted_connection_;
    };

    ConnectionRequestBuilder(std::shared_ptr<Player> player, BackendServer &to_connect)
        : player_(std::move(player)), to_connect_(&to_connect)
    {
    }

    /**
     * @see com.velocitypowered.api.proxy.ConnectionRequestBuilder#getServer
     */
    [[nodiscard]] BackendServer &getServer() const { return *to_connect_; }

    /**
     * Initiates the connection to the remote server and emits a result. The player is not told the
     * outcome.
     *
     * @see com.velocitypowered.api.proxy.ConnectionRequestBuilder#connect
     */
    [[nodiscard]] Awaitable<Result> connect() const { return internalConnect(*this); }

    /**
     * Initiates the connection to the remote server, tells the player when it fails, and emits
     * whether it succeeded.
     *
     * @see com.velocitypowered.api.proxy.ConnectionRequestBuilder#connectWithIndication
     */
    [[nodiscard]] Awaitable<bool> connectWithIndication() const { return connectWithIndication(*this); }

    /**
     * Connects to the server, telling the player when it fails, without waiting for the outcome.
     *
     * @see com.velocitypowered.api.proxy.ConnectionRequestBuilder#fireAndForget
     */
    void fireAndForget() const;

private:
    [[nodiscard]] std::optional<Status> checkServer(BackendServer &server) const;
    static Awaitable<Result> internalConnect(ConnectionRequestBuilder self);
    static Awaitable<bool> connectWithIndication(ConnectionRequestBuilder self);

    std::shared_ptr<Player> player_;
    BackendServer *to_connect_;
};

/**
 * A player on the proxy, and the backend it is on. Everything runs on the player's event loop.
 *
 * @see com.velocitypowered.api.proxy.Player
 * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer
 */
class Player : public CommandSource, public std::enable_shared_from_this<Player> {
public:
    Player(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection, GameProfile profile = {},
           std::optional<std::string> virtual_host = std::nullopt);

    /**
     * @see com.velocitypowered.api.proxy.Player#getGameProfile
     */
    [[nodiscard]] const GameProfile &getGameProfile() const { return profile_; }

    /**
     * The host the player connected through, lowercased and without its port.
     *
     * @see com.velocitypowered.api.proxy.InboundConnection#getVirtualHost
     */
    [[nodiscard]] const std::optional<std::string> &getVirtualHost() const { return virtual_host_; }

    /**
     * @see com.velocitypowered.api.proxy.Player#getUsername
     */
    [[nodiscard]] const std::string &getUsername() const { return profile_.name; }

    /**
     * @see com.velocitypowered.api.proxy.Player#getUniqueId
     */
    [[nodiscard]] const UUID &getUniqueId() const { return profile_.id; }

    [[nodiscard]] ProxyServer &getProxyServer() const { return server_; }

    [[nodiscard]] MinecraftConnection &getConnection() const { return *connection_; }

    /**
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#getConnectedServer
     */
    [[nodiscard]] const std::shared_ptr<ServerConnection> &getConnectedServer() const { return connected_server_; }

    /**
     * Sets the new server connection, and clears the connection in flight if it is the same.
     *
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#setConnectedServer
     */
    void setConnectedServer(std::shared_ptr<ServerConnection> server_connection);

    /**
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#getConnectionInFlight
     */
    [[nodiscard]] const std::shared_ptr<ServerConnection> &getConnectionInFlight() const
    {
        return connection_in_flight_;
    }

    /**
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#resetInFlightConnection
     */
    void resetInFlightConnection() { connection_in_flight_ = nullptr; }

    /**
     * Finds another server to attempt to log into, if we were unexpectedly disconnected from the server.
     *
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#getNextServerToTry
     */
    [[nodiscard]] BackendServer *getNextServerToTry(const BackendServer *current = nullptr);

    /**
     * The disconnect message for when every server in try is known to run another version than the player's,
     * or nothing while one of them could still take the player.
     */
    [[nodiscard]] std::optional<std::string> getNoServerForVersionMessage();

    /**
     * @see com.velocitypowered.api.proxy.Player#createConnectionRequest
     */
    [[nodiscard]] ConnectionRequestBuilder createConnectionRequest(BackendServer &server);

    /**
     * Handles unexpected disconnects.
     *
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#handleConnectionException(RegisteredServer,
     * Throwable, boolean)
     */
    void handleConnectionException(BackendServer &server, const std::exception &cause);

    /**
     * Handles a backend refusing or dropping the player with a reason.
     *
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#handleConnectionException(RegisteredServer,
     * DisconnectPacket, boolean)
     */
    void handleConnectionException(BackendServer &server, std::string_view reason);

    /**
     * @see net.kyori.adventure.audience.Audience#sendMessage
     */
    void sendMessage(std::string_view message) override;

    /**
     * @see endstone::core::EndstonePlayer#sendErrorMessage
     */
    void sendErrorMessage(std::string_view message) override;

    /**
     * A player has no permission set until a permission provider exists.
     *
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#getPermissionValue
     */
    [[nodiscard]] Tristate getPermissionValue(const std::string &) const override { return Tristate::Undefined; }

    [[nodiscard]] bool isInstanceOf(const std::type_info &target) const override
    {
        return target == typeid(Player) || CommandSource::isInstanceOf(target);
    }

    /**
     * The server the player is connected to, from any thread.
     *
     * @see com.velocitypowered.api.proxy.Player#getCurrentServer
     */
    [[nodiscard]] const BackendServer *getCurrentServer() const { return current_server_.load(); }

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
     * The resource packs the player joined with, each named as Bedrock names a pack: its UUID, an
     * underscore and its version.
     *
     * @see com.velocitypowered.api.proxy.Player#getAppliedResourcePacks
     */
    [[nodiscard]] const std::optional<std::vector<std::string>> &getAppliedResourcePacks() const
    {
        return applied_resource_packs_;
    }

    void setAppliedResourcePacks(std::vector<std::string> packs) { applied_resource_packs_ = std::move(packs); }

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
    [[nodiscard]] PlayerList &getPlayerList() { return player_list_; }

    /**
     * Records the soft enums the player holds with the proxy's commands, by name.
     */
    void setSoftEnums(std::map<std::string, std::vector<std::string>> soft_enums)
    {
        soft_enums_ = std::move(soft_enums);
    }

    /**
     * Tells the player how the values of the soft enums it holds changed. Runs on the player's event loop.
     *
     * @see CommandRegistry::addSoftEnumValues
     */
    void updateSoftEnums();

private:
    friend class ConnectionRequestBuilder;

    [[nodiscard]] const std::vector<std::string> &getServersToTry();

    /**
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#handleConnectionException(RegisteredServer,
     * Component, Component, boolean)
     * @see com.velocitypowered.proxy.connection.client.ConnectedPlayer#handleKickEvent
     */
    void handleKickEvent(BackendServer &server, const std::string &friendly_reason);
    static Awaitable<void> redirect(std::shared_ptr<Player> self, BackendServer &server, std::string friendly_reason);

    ProxyServer &server_;
    spdlog::logger &logger_;
    std::shared_ptr<MinecraftConnection> connection_;
    GameProfile profile_;
    std::shared_ptr<ServerConnection> connected_server_;
    std::atomic<const BackendServer *> current_server_ = nullptr;
    std::shared_ptr<ServerConnection> connection_in_flight_;
    std::optional<std::string> virtual_host_;
    std::optional<std::vector<std::string>> servers_to_try_;
    std::size_t try_index_ = 0;
    std::optional<std::vector<std::string>> applied_resource_packs_;
    ClientSettings settings_;
    PlayerList player_list_;
    std::map<std::string, std::vector<std::string>> soft_enums_;
    bool active_{true};
};

/**
 * Handles the play state between exiting the login phase and establishing the first connection to a
 * backend server: what the player sends goes to the connection in flight.
 *
 * @see com.velocitypowered.proxy.connection.client.InitialConnectSessionHandler
 */
class InitialConnectSessionHandler final
    : public HandleEach<InitialConnectSessionHandler, MinecraftSessionHandler,
                        RegisteredIn<StateRegistry::Play, Direction::Serverbound>> {
public:
    explicit InitialConnectSessionHandler(std::shared_ptr<Player> player) : player_(std::move(player)) {}

    using HandleEach::handle;

    template <typename Packet>
    bool handleEach(const Packet &)
    {
        return false;
    }

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::RequestChunkRadius, bp::MinecraftPacketIds::ClientCacheStatus>
    bool handleEach(const Packet &packet)
    {
        player_->setClientSettings(packet);
        return false;
    }

    [[nodiscard]] bool isInstanceOf(const std::type_info &target) const override
    {
        return target == typeid(InitialConnectSessionHandler) || MinecraftSessionHandler::isInstanceOf(target);
    }

    void connected() override {}
    void handleUnknown(ByteBufView batch) override;
    void readCompleted() override;
    void disconnected() override;

private:
    std::shared_ptr<Player> player_;
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

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::PlayerAuthInputPacket>
    bool handleEach(const Packet &packet)
    {
        setPlayerPosition(packet.pos.x, packet.pos.z);
        return false;
    }

    /**
     * @see com.velocitypowered.proxy.connection.client.ClientPlaySessionHandler#handle(SessionPlayerCommandPacket)
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::CommandRequest>
    bool handleEach(const Packet &packet)
    {
        return handlePlayerCommand(packet.command);
    }

    [[nodiscard]] bool isInstanceOf(const std::type_info &target) const override
    {
        return target == typeid(ClientPlaySessionHandler) || MinecraftSessionHandler::isInstanceOf(target);
    }

    void connected() override {}
    void handleUnknown(ByteBufView batch) override;
    void readCompleted() override;
    void disconnected() override;

    /**
     * Rate limits a command, then runs it on the proxy if it is one, and returns whether the proxy took it
     * rather than the backend.
     *
     * @see com.velocitypowered.proxy.protocol.packet.chat.RateLimitedCommandHandler#handlePlayerCommand
     */
    [[nodiscard]] bool handlePlayerCommand(const std::string &command);

    /**
     * Handles the StartGame packet. This function is responsible for handling the client-side
     * switching servers in endgate.
     *
     * @see com.velocitypowered.proxy.connection.client.ClientPlaySessionHandler#handleBackendJoinGame
     */
    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::StartGame>
    void handleBackendStartGame(const Packet &start_game, ServerConnection &destination);

    void handleBackendPlayerSpawn(ServerConnection &destination);

    template <typename Packet>
        requires IsPacket<Packet, bp::MinecraftPacketIds::StartGame>
    [[nodiscard]] std::optional<std::string_view> findIncompatibility(const Packet &start_game) const;

    /**
     * @see com.velocitypowered.proxy.connection.client.ClientPlaySessionHandler#getServerBossBars
     */
    [[nodiscard]] std::vector<bp::ActorUniqueID> &getServerBossBars() { return server_boss_bars_; }

    [[nodiscard]] bp::ActorRuntimeID getRuntimeId() const { return runtime_id_; }
    [[nodiscard]] bool hasSwitched() const { return switched_; }
    [[nodiscard]] std::int32_t &getDimension() { return dimension_; }
    [[nodiscard]] std::set<std::pair<std::int32_t, std::int32_t>> &getServerChunks() { return server_chunks_; }
    [[nodiscard]] std::set<bp::ActorUniqueID> &getServerActors() { return server_actors_; }
    [[nodiscard]] std::set<std::string> &getServerObjectives() { return server_objectives_; }
    [[nodiscard]] std::set<std::int32_t> &getServerEffects() { return server_effects_; }
    [[nodiscard]] std::set<std::uint64_t> &getServerShapes() { return server_shapes_; }
    [[nodiscard]] std::optional<std::pair<std::int32_t, std::int32_t>> &getServerContainer()
    {
        return server_container_;
    }
    [[nodiscard]] std::map<std::int32_t, std::string> &getSessionRegistries() { return session_registries_; }

    void setChunkRadius(std::int32_t radius);
    void setChunkPublisher(std::int32_t x, std::int32_t z, std::uint32_t radius);
    void setSpawnPosition(bool respawn, std::int32_t x, std::int32_t z);

private:
    struct ChunkView {
        std::int32_t x;
        std::int32_t z;
        std::int32_t radius;
    };

    void setPlayerPosition(float x, float z);
    void pruneServerChunks();

    /**
     * @see com.velocitypowered.proxy.protocol.packet.chat.session.SessionCommandHandler#handlePlayerCommandInternal
     * @see com.velocitypowered.proxy.protocol.packet.chat.CommandHandler#queueCommandResult
     */
    [[nodiscard]] bool handlePlayerCommandInternal(const std::string &command);

    spdlog::logger &logger_;
    std::shared_ptr<Player> player_;
    int failed_attempts_ = 0;
    bool spawned_{false};
    bool switched_{false};
    bool awaiting_spawn_{false};
    bool fade_lifted_ = false;
    bp::ActorRuntimeID runtime_id_;
    bp::ActorUniqueID unique_id_;
    std::int32_t dimension_{0};
    std::set<std::pair<std::int32_t, std::int32_t>> server_chunks_;
    std::vector<bp::ActorUniqueID> server_boss_bars_;
    std::set<bp::ActorUniqueID> server_actors_;
    std::set<std::string> server_objectives_;
    std::set<std::int32_t> server_effects_;
    std::set<std::uint64_t> server_shapes_;
    std::optional<std::pair<std::int32_t, std::int32_t>> server_container_;
    std::optional<std::pair<std::int32_t, std::int32_t>> player_chunk_;
    std::optional<std::int32_t> chunk_radius_;
    std::optional<ChunkView> publisher_view_;
    std::optional<ChunkView> world_spawn_view_;
    std::optional<ChunkView> respawn_view_;
    std::uint64_t block_registry_checksum_ = 0;
    bool block_network_ids_are_hashes_ = false;
    bool server_auth_block_breaking_ = false;
    std::string experiments_;
    std::map<std::int32_t, std::string> session_registries_;
};

}  // namespace endgate
