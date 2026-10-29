#include "endgate/connection/backend.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/cancel_after.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/udp.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
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
#include "endgate/server.h"

namespace endgate {

ServerConnection::ServerConnection(BackendServer &server, std::shared_ptr<Player> player)
    : server_(server), player_(std::move(player)), logger_(LogManager::getLogger("ServerConnection"))
{
}

void ServerConnection::connect()
{
    asio::co_spawn(player_->getConnection().eventLoop().getExecutor(), open(shared_from_this()), asio::detached);
}

Awaitable<void> ServerConnection::open(std::shared_ptr<ServerConnection> self)
{
    constexpr std::chrono::seconds resolve_timeout{2};

    const auto &info = self->server_.getServerInfo();
    auto &player = *self->player_;
    auto &endpoint = player.getConnection().getEndpoint();
    const auto &player_address = player.getConnection().getRemoteAddress();

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
        co_return;
    }
    if (!address || !transport) {
        self->logger_.error("[connected player] {} ({}): unable to connect to server {}: {}",
                            self->player_->getGameProfile().name, player_address, info.getName(),
                            address ? "no NetherNet or RakNet response" : "can't resolve the address");
        player.disconnect();
        co_return;
    }
    if (*transport == ServerInfo::Transport::NetherNet) {
        const auto connection = std::make_shared<NetherNetConnection<nethernet::DialerSession>>(
            endpoint, *address, endpoint.getCertificate());
        connection->setSessionHandler(std::make_shared<LoginSessionHandler>(self));
        self->connection_ = connection;
        endpoint.registerConnection(connection, std::nullopt);
        self->logger_.debug("[server connection] {} -> {}: connecting over NetherNet",
                            self->player_->getGameProfile().name, info.getName());
        connection->connect(info.getHost(), info.getPort(), endpoint.getOfferIdentity());
        co_return;
    }

    const auto guid = std::mt19937_64{std::random_device{}()}();
    const auto connection = std::make_shared<RakNetConnection>(endpoint, *address, guid, raknet::Clock::now());
    connection->setSessionHandler(std::make_shared<LoginSessionHandler>(self));
    self->connection_ = connection;
    endpoint.registerConnection(connection, guid);
    self->logger_.debug("[server connection] {} -> {}: connecting over RakNet", self->player_->getGameProfile().name,
                        info.getName());
    connection->connect();
}

void ServerConnection::disconnect()
{
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
    : logger_(LogManager::getLogger("BackendPlaySessionHandler")), server_connection_(std::move(server_connection))
{
}

void BackendPlaySessionHandler::activated()
{
    connected_ = true;
    auto &player = server_connection_->getPlayer().getConnection();
    if (player.getEndpoint().getServer().getConfiguration().isLogPlayerConnections()) {
        logger_.info("[server connection] {} -> {} has connected",
                     server_connection_->getPlayer().getGameProfile().name,
                     server_connection_->getServer().getServerInfo().getName());
    }
    server_connection_->connected();
}

void BackendPlaySessionHandler::handleUnknown(ByteBufView batch)
{
    server_connection_->getPlayer().getConnection().delayedWrite(batch.copy());
}

void BackendPlaySessionHandler::readCompleted()
{
    server_connection_->getPlayer().getConnection().flush();
}

void BackendPlaySessionHandler::disconnected()
{
    auto &player = server_connection_->getPlayer().getConnection();
    if (connected_ && player.getEndpoint().getServer().getConfiguration().isLogPlayerConnections()) {
        logger_.info("[server connection] {} -> {} has disconnected",
                     server_connection_->getPlayer().getGameProfile().name,
                     server_connection_->getServer().getServerInfo().getName());
    }
    if (!connected_) {
        server_connection_->getServer().forgetTransport();
    }
    server_connection_->getPlayer().disconnect();
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

    struct glaze {
        using T = SelfSignedClaims;
        static constexpr auto value = glz::object("cpk", &T::cpk, "exp", &T::exp, "iat", &T::iat);
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

LoginSessionHandler::LoginSessionHandler(std::shared_ptr<ServerConnection> server_connection)
    : logger_(LogManager::getLogger("LoginSessionHandler")), server_connection_(std::move(server_connection))
{
}

void LoginSessionHandler::connected()
{
    auto *const backend = server_connection_->getConnection();
    const auto version = server_connection_->getPlayer().getConnection().getProtocolVersion();
    backend->setState(StateRegistry::Login);
    backend->setProtocolVersion(version);
    backend->write(bp::RequestNetworkSettingsPacket{.client_network_version = version.getProtocol()});
}

bool LoginSessionHandler::handle(const bp::NetworkSettingsPacket &packet)
{
    constexpr std::chrono::hours token_lifetime{1};

    auto *const backend = server_connection_->getConnection();
    backend->setCompressionThreshold(packet.compression_threshold, packet.compression_algorithm);

    auto *const key = backend->getEndpoint().getOfferIdentity()->getKey();
    unsigned char *der = nullptr;
    const auto der_length = i2d_PUBKEY(key, &der);
    const std::unique_ptr<unsigned char, decltype([](unsigned char *p) { OPENSSL_free(p); })> owned_der(der);
    if (der_length <= 0) {
        server_connection_->getPlayer().disconnect();
        return true;
    }
    const auto cpk = base64Encode(std::as_bytes(std::span{der, static_cast<std::size_t>(der_length)}));
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    std::string header;
    std::string claims;
    std::ignore = glz::write_json(TokenHeader{.alg = std::string(curveOf(key)->algorithm), .x5u = cpk}, header);
    std::ignore = glz::write_json(
        SelfSignedClaims{.cpk = cpk, .exp = now + token_lifetime / std::chrono::seconds{1}, .iat = now}, claims);
    const auto token = signToken(key, header, claims);
    const auto client_data = signToken(key, header, server_connection_->getPlayer().getGameProfile().client_data);
    std::string authentication;
    if (!token || !client_data ||
        glz::write_json(LoginAuthentication{.authentication_type = 2, .token = *token}, authentication)) {
        server_connection_->getPlayer().disconnect();
        return true;
    }
    std::string request;
    appendLongString(request, authentication);
    appendLongString(request, *client_data);
    backend->write(bp::LoginPacket{.client_network_version = backend->getProtocolVersion().getProtocol(),
                                   .connection_request = std::move(request)});
    return true;
}

bool LoginSessionHandler::handle(const bp::ServerToClientHandshakePacket &packet)
{
    auto *const backend = server_connection_->getConnection();
    const auto token = JsonWebToken::decode(packet.token).value_or(JsonWebToken{});
    const auto server_key = decodeSubjectPublicKeyInfo(token.x5u);
    HandshakeClaims claims;
    if (!server_key || !token.verify(server_key.get()) || glz::read<LENIENT_READ>(claims, token.claims)) {
        logger_.error("[server connection] {} -> {}: the server's handshake doesn't verify",
                      server_connection_->getPlayer().getGameProfile().name,
                      server_connection_->getServer().getServerInfo().getName());
        server_connection_->getPlayer().disconnect();
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
        server_connection_->getPlayer().disconnect();
        return true;
    }
    if (!backend->isTransportEncrypted()) {
        backend->enableEncryption(*key);
    }
    backend->write(bp::ClientToServerHandshakePacket{});
    return true;
}

bool LoginSessionHandler::handle(const bp::PlayStatusPacket &packet)
{
    auto *const backend = server_connection_->getConnection();
    if (packet.status != bp::PlayStatus::LoginSuccess) {
        server_connection_->getPlayer().getConnection().write(packet);
        server_connection_->getPlayer().disconnect();
        return true;
    }
    logged_in_ = true;
    backend->setState(StateRegistry::Play);
    backend->setSessionHandler(std::make_shared<BackendPlaySessionHandler>(server_connection_));
    return true;
}

void LoginSessionHandler::handleGeneric(ByteBufView packet)
{
    server_connection_->getPlayer().getConnection().write(packet.copy());
    server_connection_->getPlayer().disconnect();
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
    server_connection_->getPlayer().disconnect();
}

}  // namespace endgate
