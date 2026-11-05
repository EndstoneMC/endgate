#include "endgate/connection/backend.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/cancel_after.hpp>
#include <asio/ip/udp.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <bedrock/protocol/camera.h>
#include <glaze/json.hpp>
#include <openssl/x509.h>

#include "endgate/awaitable.h"
#include "endgate/backend.h"
#include "endgate/connection/nethernet.h"
#include "endgate/connection/player.h"
#include "endgate/connection/raknet.h"
#include "endgate/crypto/encryption.h"
#include "endgate/crypto/jose.h"
#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"
#include "endgate/network/version.h"
#include "endgate/server.h"

namespace endgate {

ServerConnection::ServerConnection(BackendServer &server, std::shared_ptr<Player> player)
    : server_(server), player_(std::move(player)), logger_(LogManager::getLogger(*this))
{
}

Awaitable<ConnectionRequestBuilder::Result> ServerConnection::open(std::shared_ptr<ServerConnection> self)
{
    constexpr std::chrono::seconds resolve_timeout{2};

    const auto &info = self->server_.getServerInfo();
    auto &player = *self->player_;
    auto &endpoint = player.getConnection().getEndpoint();
    const auto result_future = std::make_shared<ConnectionResultFuture>(co_await asio::this_coro::executor);

    std::optional<asio::ip::udp::endpoint> address;
    asio::error_code ec;
    if (const auto literal = asio::ip::make_address(info.getHost(), ec); !ec) {
        address.emplace(literal, info.getPort());
    }
    else {
        asio::ip::udp::resolver resolver(co_await asio::this_coro::executor);
        const auto [error, results] =
            co_await resolver.async_resolve(info.getHost(), std::to_string(info.getPort()),
                                            asio::cancel_after(resolve_timeout, asio::as_tuple(asio::use_awaitable)));
        if (!error && !results.empty()) {
            address = results.begin()->endpoint();
        }
    }

    std::optional<ServerInfo::Transport> transport;
    if (address && player.isActive()) {
        transport = co_await self->server_.detectTransport(endpoint.getEventLoop().getIoContext(), *address);
    }
    if (!player.isActive()) {
        throw std::runtime_error("the player left");
    }
    if (!address || !transport) {
        throw std::runtime_error(address ? "no NetherNet or RakNet response" : "can't resolve the address");
    }
    const auto player_protocol = player.getConnection().getProtocolVersion().getProtocol();
    if (const auto backend_protocol = self->server_.getProtocol();
        backend_protocol && *backend_protocol != player_protocol) {
        co_return ConnectionRequestBuilder::Result::forDisconnect(std::format("it runs Minecraft {}, not {}",
                                                                              describeProtocol(*backend_protocol),
                                                                              describeProtocol(player_protocol)),
                                                                  self->server_);
    }
    if (const auto connect_timeout = endpoint.getServer().getConfiguration().getConnectTimeout(); connect_timeout > 0) {
        self->connect_timeout_ = player.getConnection().eventLoop().schedule(
            [self, result_future] {
                if (self->established_) {
                    return;
                }
                result_future->completeExceptionally(
                    std::make_exception_ptr(std::runtime_error("connection timed out")));
                self->disconnect();
            },
            std::chrono::milliseconds{connect_timeout});
    }
    if (*transport == ServerInfo::Transport::NetherNet) {
        const auto connection = std::make_shared<NetherNetConnection<nethernet::DialerSession>>(
            endpoint, *address, endpoint.getCertificate());
        connection->setSessionHandler(std::make_shared<LoginSessionHandler>(self, result_future));
        self->connection_ = connection;
        endpoint.registerConnection(connection, std::nullopt);
        self->logger_.debug("[server connection] {} -> {}: connecting over NetherNet",
                            self->player_->getGameProfile().name, info.getName());
        connection->connect(info.getHost(), info.getPort(), endpoint.getOfferIdentity());
    }
    else {
        const auto guid = std::mt19937_64{std::random_device{}()}();
        const auto connection = std::make_shared<RakNetConnection>(endpoint, *address, guid, raknet::Clock::now());
        connection->setSessionHandler(std::make_shared<LoginSessionHandler>(self, result_future));
        self->connection_ = connection;
        endpoint.registerConnection(connection, guid);
        self->logger_.debug("[server connection] {} -> {}: connecting over RakNet",
                            self->player_->getGameProfile().name, info.getName());
        connection->connect();
    }
    co_return co_await result_future->get();
}

MinecraftConnection &ServerConnection::ensureConnected() const
{
    if (!connection_) {
        throw std::logic_error("Not connected to server!");
    }
    return *connection_;
}

void ServerConnection::connectionEstablished()
{
    established_ = true;
    if (const auto timeout = std::exchange(connect_timeout_, nullptr)) {
        timeout->cancel();
    }
}

void ServerConnection::disconnect()
{
    graceful_disconnect_ = true;
    if (const auto timeout = std::exchange(connect_timeout_, nullptr)) {
        timeout->cancel();
    }
    pending_.clear();
    pending_bytes_ = 0;
    if (connection_) {
        connection_->close();
    }
}

void ServerConnection::write(ByteBuf batch)
{
    constexpr std::size_t max_pending_bytes = 64 * 1024;

    if (connected_) {
        connection_->delayedWrite(std::move(batch));
        return;
    }
    if (pending_bytes_ > max_pending_bytes) {
        return;
    }
    pending_bytes_ += batch.readableBytes();
    if (pending_bytes_ > max_pending_bytes) {
        logger_.warn("[server connection] {} -> {}: more than {} bytes arrived before the server was up, "
                     "disconnecting",
                     player_->getGameProfile().name, server_.getServerInfo().getName(), max_pending_bytes);
        pending_.clear();
        player_->disconnect();
        return;
    }
    pending_.push_back(std::move(batch));
}

void ServerConnection::flush()
{
    if (connected_) {
        connection_->flush();
    }
}

void ServerConnection::connected()
{
    if (!player_->isActive() || !connection_) {
        return;
    }
    connected_ = true;
    for (auto &batch : std::exchange(pending_, {})) {
        connection_->delayedWrite(std::move(batch));
    }
    connection_->flush();
    pending_bytes_ = 0;
}

BackendPlaySessionHandler::BackendPlaySessionHandler(std::shared_ptr<ServerConnection> server_connection)
    : logger_(LogManager::getLogger(*this)), server_connection_(std::move(server_connection)),
      player_session_handler_([this] {
          const auto &active = server_connection_->getPlayer().getConnection().getActiveSessionHandler();
          return std::shared_ptr<ClientPlaySessionHandler>(active,
                                                           active ? active->as<ClientPlaySessionHandler>() : nullptr);
      }())
{
    if (!player_session_handler_) {
        throw std::logic_error("Initializing BackendPlaySessionHandler with no backing client play session handler!");
    }
}

bool BackendPlaySessionHandler::beforeHandle()
{
    if (!server_connection_->isActive()) {
        server_connection_->disconnect();
        return true;
    }
    return false;
}

void BackendPlaySessionHandler::handleUnknown(ByteBufView batch)
{
    server_connection_->getPlayer().getConnection().delayedWrite(batch.copy());
}

void BackendPlaySessionHandler::readCompleted()
{
    server_connection_->getPlayer().getConnection().flush();
}

bool BackendPlaySessionHandler::handleTransfer(std::string_view host, std::uint16_t port)
{
    auto &player = server_connection_->getPlayer();
    for (auto *const server : player.getConnection().getEndpoint().getServer().getAllServers()) {
        const auto &info = server->getServerInfo();
        if (info.getHost() == host && info.getPort() == port) {
            player.createConnectionRequest(*server).fireAndForget();
            return true;
        }
    }
    return false;
}

bool BackendPlaySessionHandler::handleAvailableCommands(const bp::AvailableCommandsPacket &packet)
{
    auto &player = server_connection_->getPlayer();
    auto &proxy = player.getConnection().getEndpoint().getServer();
    if (!proxy.getConfiguration().isAnnounceProxyCommands()) {
        return false;
    }
    bp::AvailableCommandsPacket injected = packet;
    player.setSoftEnums(proxy.getCommandManager().getInjector().inject(
        injected, player.shared_from_this(), player.getConnection().getProtocolVersion().getProtocol()));
    player.getConnection().delayedWrite(std::move(injected));
    return true;
}

void BackendPlaySessionHandler::disconnected()
{
    auto &player = server_connection_->getPlayer().getConnection();
    if (player.getEndpoint().getServer().getConfiguration().isLogPlayerConnections()) {
        logger_.info("[server connection] {} -> {} has disconnected",
                     server_connection_->getPlayer().getGameProfile().name,
                     server_connection_->getServer().getServerInfo().getName());
    }
    if (server_connection_->isGracefulDisconnect()) {
        return;
    }
    if (player.getEndpoint().getServer().getConfiguration().isFailoverOnUnexpectedServerDisconnect()) {
        server_connection_->getPlayer().handleConnectionException(server_connection_->getServer(),
                                                                  ConnectionMessages::INTERNAL_SERVER_CONNECTION_ERROR);
        return;
    }
    server_connection_->getPlayer().disconnect(DisconnectFailReason::NoReason,
                                               std::string{ConnectionMessages::INTERNAL_SERVER_CONNECTION_ERROR});
}

TransitionSessionHandler::TransitionSessionHandler(std::shared_ptr<ServerConnection> server_connection,
                                                   std::shared_ptr<ConnectionResultFuture> result_future)
    : logger_(LogManager::getLogger(*this)), server_connection_(std::move(server_connection)),
      result_future_(std::move(result_future))
{
}

bool TransitionSessionHandler::beforeHandle()
{
    if (!server_connection_->isActive()) {
        server_connection_->disconnect();
        return true;
    }
    return false;
}

void TransitionSessionHandler::activated()
{
    constexpr float fade_in_seconds = 0.5F;
    constexpr float hold_seconds = 5.0F;
    constexpr float fade_out_seconds = 0.5F;

    auto &player = server_connection_->getPlayer().getConnection();
    if (player.getEndpoint().getServer().getConfiguration().isLogPlayerConnections()) {
        logger_.info("[server connection] {} -> {} has connected",
                     server_connection_->getPlayer().getGameProfile().name,
                     server_connection_->getServer().getServerInfo().getName());
    }
    server_connection_->connected();
    if (!isSwitching()) {
        return;
    }
    visitProtocolVersion(player.getProtocolVersion().getProtocol(), [&]<int Version>() {
        PacketOf<Version, bp::MinecraftPacketIds::CameraInstruction> camera;
        auto &fade = camera.camera_instruction.fade.emplace();
        fade.time = typename decltype(fade.time)::value_type{
            .fade_in_time = fade_in_seconds, .hold_time = hold_seconds, .fade_out_time = fade_out_seconds};
        fade.color.emplace();
        player.write(std::move(camera));
        return true;
    });
}

void TransitionSessionHandler::handleUnknown(ByteBufView batch)
{
    auto &player = server_connection_->getPlayer();
    if (isSwitching() && player.getConnectedServer() != server_connection_) {
        return;
    }
    player.getConnection().delayedWrite(batch.copy());
}

bool TransitionSessionHandler::isSwitching() const
{
    const auto &active = server_connection_->getPlayer().getConnection().getActiveSessionHandler();
    return active && active->is<ClientPlaySessionHandler>();
}

void TransitionSessionHandler::readCompleted()
{
    server_connection_->getPlayer().getConnection().flush();
}

void TransitionSessionHandler::disconnected()
{
    constexpr float fade_out_seconds = 0.5F;

    auto &player = server_connection_->getPlayer().getConnection();
    if (player.getEndpoint().getServer().getConfiguration().isLogPlayerConnections()) {
        logger_.info("[server connection] {} -> {} has disconnected",
                     server_connection_->getPlayer().getGameProfile().name,
                     server_connection_->getServer().getServerInfo().getName());
    }
    if (server_connection_->getPlayer().isActive() && isSwitching()) {
        visitProtocolVersion(player.getProtocolVersion().getProtocol(), [&]<int Version>() {
            PacketOf<Version, bp::MinecraftPacketIds::CameraInstruction> camera;
            auto &fade = camera.camera_instruction.fade.emplace();
            fade.time = typename decltype(fade.time)::value_type{.fade_out_time = fade_out_seconds};
            fade.color.emplace();
            player.write(std::move(camera));
            return true;
        });
    }
    result_future_->complete(ConnectionRequestBuilder::Result::forDisconnect(
        std::string{ConnectionMessages::INTERNAL_SERVER_CONNECTION_ERROR}, server_connection_->getServer()));
}

}  // namespace endgate

namespace endgate {

namespace {

constexpr glz::opts LENIENT_READ{.error_on_unknown_keys = false, .error_on_missing_keys = true};

struct LoginAuthentication {
    int authentication_type;
    std::string token;

    struct glaze {
        using T = LoginAuthentication;
        static constexpr auto value = glz::object("AuthenticationType", &T::authentication_type, "Token", &T::token);
    };
};

struct TokenHeader {
    std::string alg;
    std::string x5u;

    struct glaze {
        using T = TokenHeader;
        static constexpr auto value = glz::object("alg", &T::alg, "x5u", &T::x5u);
    };
};

struct SelfSignedClaims {
    std::string cpk;
    std::int64_t exp;
    std::int64_t iat;
    std::string xname;
    std::string leguuid;

    struct glaze {
        using T = SelfSignedClaims;
        static constexpr auto value =
            glz::object("cpk", &T::cpk, "exp", &T::exp, "iat", &T::iat, "xname", &T::xname, "leguuid", &T::leguuid);
    };
};

struct ForwardingHeader {
    std::string alg;
    std::string kid;

    struct glaze {
        using T = ForwardingHeader;
        static constexpr auto value = glz::object("alg", &T::alg, "kid", &T::kid);
    };
};

struct PlayerInfo {
    int version;
    std::string address;

    struct glaze {
        using T = PlayerInfo;
        static constexpr auto value = glz::object("version", &T::version, "address", &T::address);
    };
};

struct HandshakeClaims {
    std::string salt;

    struct glaze {
        using T = HandshakeClaims;
        static constexpr auto value = glz::object("salt", &T::salt);
    };
};

std::span<const std::byte> bytesOf(std::string_view text)
{
    return std::as_bytes(std::span{text.data(), text.size()});
}

std::optional<std::string> signToken(EVP_PKEY *key, const std::string &header, std::string_view claims)
{
    const auto signing_input = base64UrlEncode(bytesOf(header)) + "." + base64UrlEncode(bytesOf(claims));
    const auto signature = signJws(key, signing_input);
    if (!signature) {
        return std::nullopt;
    }
    return signing_input + "." + *signature;
}

void appendLongString(std::string &out, std::string_view text)
{
    const auto length = static_cast<std::uint32_t>(text.size());
    for (std::size_t i = 0; i < sizeof(length); ++i) {
        out.push_back(static_cast<char>(length >> (8 * i)));
    }
    out.append(text);
}

}  // namespace

std::optional<std::string> ServerConnection::createForwardingData(
    const nethernet::ServerIdentityAssertionGenerator &identity, std::string_view address, const GameProfile &profile,
    std::chrono::system_clock::time_point now)
{
    constexpr std::chrono::hours token_lifetime{1};

    const auto issued = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    std::map<std::string, glz::raw_json> claims;
    std::string cpk;
    std::string player_info;
    std::string header;
    std::string body;
    if (glz::read_json(claims, profile.identity_claims) || glz::write_json(identity.getPublicKey(), cpk) ||
        glz::write_json(PlayerInfo{.version = MODERN_FORWARDING_DEFAULT, .address = std::string(address)},
                        player_info) ||
        glz::write_json(
            ForwardingHeader{.alg = std::string(curveOf(identity.getKey())->algorithm), .kid = identity.getPublicKey()},
            header)) {
        return std::nullopt;
    }
    claims["cpk"] = std::move(cpk);
    claims["exp"] = std::to_string(issued + token_lifetime / std::chrono::seconds{1});
    claims["iat"] = std::to_string(issued);
    claims[std::string(PLAYER_INFO_CLAIM)] = std::move(player_info);
    if (glz::write_json(claims, body)) {
        return std::nullopt;
    }
    return signToken(identity.getKey(), header, body);
}

LoginSessionHandler::LoginSessionHandler(std::shared_ptr<ServerConnection> server_connection,
                                         std::shared_ptr<ConnectionResultFuture> result_future)
    : logger_(LogManager::getLogger(*this)), server_connection_(std::move(server_connection)),
      result_future_(std::move(result_future))
{
}

void LoginSessionHandler::connected()
{
    server_connection_->connectionEstablished();
    auto *const backend = server_connection_->getConnection();
    const auto version = server_connection_->getPlayer().getConnection().getProtocolVersion();
    backend->setState(StateRegistry::Login);
    backend->setProtocolVersion(version);
    backend->write(bp::RequestNetworkSettingsPacket{.client_network_version = version.getProtocol()});
}

bool LoginSessionHandler::handleEach(const bp::NetworkSettingsPacket &packet)
{
    constexpr std::chrono::hours token_lifetime{1};

    auto *const backend = server_connection_->getConnection();
    backend->setCompressionThreshold(packet.compression_threshold, packet.compression_algorithm);

    const auto &identity = *backend->getEndpoint().getOfferIdentity();
    auto *const key = identity.getKey();
    const auto &cpk = identity.getPublicKey();
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    std::string header;
    std::string claims;
    std::ignore = glz::write_json(TokenHeader{.alg = std::string(curveOf(key)->algorithm), .x5u = cpk}, header);
    const auto &player = server_connection_->getPlayer();
    const auto &profile = player.getGameProfile();
    std::ignore = glz::write_json(SelfSignedClaims{.cpk = cpk,
                                                   .exp = now + token_lifetime / std::chrono::seconds{1},
                                                   .iat = now,
                                                   .xname = profile.name,
                                                   .leguuid = profile.id.toString()},
                                  claims);
    std::map<std::string, glz::raw_json> client_data_claims;
    std::string name;
    std::string self_signed_id;
    std::string forwarded_client_data;
    if (glz::read_json(client_data_claims, profile.client_data) || glz::write_json(profile.name, name) ||
        glz::write_json(profile.id.toString(), self_signed_id)) {
        result_future_->completeExceptionally(
            std::make_exception_ptr(std::runtime_error("can't read the player's client data")));
        server_connection_->disconnect();
        return true;
    }
    client_data_claims["ThirdPartyName"] = std::move(name);
    client_data_claims["SelfSignedId"] = std::move(self_signed_id);
    std::ignore = glz::write_json(client_data_claims, forwarded_client_data);
    const auto modern =
        player.getConnection().getEndpoint().getServer().getConfiguration().getPlayerInfoForwardingMode() ==
        PlayerInfoForwarding::Modern;
    std::optional<std::string> token;
    if (modern) {
        auto ip = player.getConnection().getRemoteIp();
        if (ip.is_v6()) {
            auto v6 = ip.to_v6();
            v6.scope_id(0);
            ip = v6;
        }
        token =
            ServerConnection::createForwardingData(identity, ip.to_string(), profile, std::chrono::system_clock::now());
    }
    else {
        token = signToken(key, header, claims);
    }
    const auto client_data = signToken(key, header, forwarded_client_data);
    std::string authentication;
    if (!token || !client_data ||
        glz::write_json(LoginAuthentication{.authentication_type = modern ? 0 : 2, .token = *token}, authentication)) {
        result_future_->completeExceptionally(std::make_exception_ptr(std::runtime_error("can't sign the login")));
        server_connection_->disconnect();
        return true;
    }
    std::string request;
    appendLongString(request, authentication);
    appendLongString(request, *client_data);
    backend->write(bp::LoginPacket{.client_network_version = backend->getProtocolVersion().getProtocol(),
                                   .connection_request = std::move(request)});
    return true;
}

bool LoginSessionHandler::handleEach(const bp::ServerToClientHandshakePacket &packet)
{
    auto *const backend = server_connection_->getConnection();
    const auto token = JsonWebToken::decode(packet.token).value_or(JsonWebToken{});
    const auto server_key = decodeSubjectPublicKeyInfo(token.x5u);
    HandshakeClaims claims;
    if (!server_key || !token.verify(server_key.get()) || glz::read<LENIENT_READ>(claims, token.claims)) {
        result_future_->completeExceptionally(
            std::make_exception_ptr(std::runtime_error("the server's handshake doesn't verify")));
        server_connection_->disconnect();
        return true;
    }
    auto salt = base64Decode(claims.salt);
    if (!salt) {
        salt = base64UrlDecode(claims.salt);
    }
    const auto key =
        salt ? deriveEncryptionKey(backend->getEndpoint().getOfferIdentity()->getKey(), server_key.get(), *salt)
             : std::nullopt;
    if (!key) {
        result_future_->completeExceptionally(
            std::make_exception_ptr(std::runtime_error("can't derive the encryption key")));
        server_connection_->disconnect();
        return true;
    }
    if (!backend->isTransportEncrypted()) {
        backend->enableEncryption(*key);
    }
    backend->write(bp::ClientToServerHandshakePacket{});
    return true;
}

bool LoginSessionHandler::handleEach(const bp::PlayStatusPacket &packet)
{
    auto *const backend = server_connection_->getConnection();
    if (packet.status != bp::PlayStatus::LoginSuccess) {
        std::optional<std::string> reason;
        switch (packet.status) {
        case bp::PlayStatus::LoginFailedClientOld:
            reason = "Could not connect: Outdated client!";
            break;
        case bp::PlayStatus::LoginFailedServerOld:
            reason = "The host is using an older version of Minecraft. Everyone should update to the latest version "
                     "of Minecraft and try again.";
            break;
        case bp::PlayStatus::LoginFailedInvalidTenant:
            reason = "Unable to connect to the world. Please check your join code and try again.";
            break;
        case bp::PlayStatus::LoginFailedEditionMismatchEduToVanilla:
            reason = "The server is not running Minecraft Education. Failed to connect.";
            break;
        case bp::PlayStatus::LoginFailedEditionMismatchVanillaToEdu:
            reason = "The server is running an incompatible edition of Minecraft. Failed to connect.";
            break;
        case bp::PlayStatus::LoginFailedServerFullSubClient:
            reason = "Wow this server is popular! Check back later to see if space opens up.";
            break;
        case bp::PlayStatus::LoginFailedEditorMismatchEditorToVanilla:
            reason = "The server is not in Editor Mode. Failed to connect.";
            break;
        case bp::PlayStatus::LoginFailedEditorMismatchVanillaToEditor:
            reason = "The server is in Editor Mode. Failed to connect.";
            break;
        default:
            break;
        }
        result_future_->complete(
            ConnectionRequestBuilder::Result::forDisconnect(std::move(reason), server_connection_->getServer()));
        server_connection_->disconnect();
        return true;
    }
    if (!server_connection_->isActive()) {
        server_connection_->disconnect();
        return true;
    }
    logged_in_ = true;
    backend->setState(StateRegistry::Play);
    backend->setSessionHandler(std::make_shared<TransitionSessionHandler>(server_connection_, result_future_));
    return true;
}

void LoginSessionHandler::handleUnknown(ByteBufView packet)
{
    logger_.debug("[server connection] {} -> {}: dropping {} bytes before the login completed",
                  server_connection_->getPlayer().getGameProfile().name,
                  server_connection_->getServer().getServerInfo().getName(), packet.readableBytes());
}

void LoginSessionHandler::disconnected()
{
    if (!logged_in_) {
        server_connection_->getServer().forgetTransport();
    }
    result_future_->completeExceptionally(
        std::make_exception_ptr(std::runtime_error("the connection to the remote server was unexpectedly closed")));
}

}  // namespace endgate
