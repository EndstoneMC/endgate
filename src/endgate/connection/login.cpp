#include "endgate/connection/login.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

#include <glaze/json.hpp>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include "endgate/crypto/encryption.h"
#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"
#include "endgate/network/version.h"
#include "endgate/server.h"

namespace endgate {

namespace {

constexpr glz::opts LENIENT_READ{.error_on_unknown_keys = false, .error_on_missing_keys = true};

struct AuthenticationInfo {
    std::optional<int> authentication_type;
    std::optional<std::string> token;

    struct glaze {
        using T = AuthenticationInfo;
        static constexpr auto value = glz::object("AuthenticationType", &T::authentication_type, "Token", &T::token);
    };
};

struct ClientDataName {
    std::optional<std::string> third_party_name;

    struct glaze {
        using T = ClientDataName;
        static constexpr auto value = glz::object("ThirdPartyName", &T::third_party_name);
    };
};

struct HandshakeHeader {
    std::string alg;
    std::string x5u;

    struct glaze {
        using T = HandshakeHeader;
        static constexpr auto value = glz::object("alg", &T::alg, "x5u", &T::x5u);
    };
};

struct HandshakeClaims {
    std::string salt;

    struct glaze {
        using T = HandshakeClaims;
        static constexpr auto value = glz::object("salt", &T::salt);
    };
};

std::optional<std::string> readLongString(ByteBufView &in)
{
    const auto length = in.readIntLE();
    if (length < 0 || static_cast<std::size_t>(length) > in.readableBytes()) {
        return std::nullopt;
    }
    const auto bytes = in.readSlice(static_cast<std::size_t>(length)).nioBuffer();
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

std::span<const std::byte> bytesOf(std::string_view text)
{
    return std::as_bytes(std::span{text.data(), text.size()});
}

}  // namespace

std::optional<ConnectionRequest> ConnectionRequest::fromString(std::string_view request)
{
    ByteBufView in{bytesOf(request)};
    try {
        const auto info = readLongString(in);
        const auto data = readLongString(in);
        AuthenticationInfo parsed;
        if (!info || !data || glz::read<LENIENT_READ>(parsed, *info)) {
            return std::nullopt;
        }
        auto token = JsonWebToken::decode(parsed.token.value_or(""));
        auto client_data = JsonWebToken::decode(*data, MAX_CLIENT_DATA_SIZE);
        if (!token || !client_data) {
            return std::nullopt;
        }
        return ConnectionRequest{.authentication_type = parsed.authentication_type.value_or(-1),
                                 .token = std::move(*token),
                                 .client_data = std::move(*client_data)};
    }
    catch (const std::out_of_range &) {
        return std::nullopt;
    }
}

InitialLoginSessionHandler::InitialLoginSessionHandler(ProxyServer &server,
                                                       std::shared_ptr<MinecraftConnection> connection,
                                                       std::optional<GameServerToken> offer_identity)
    : server_(server), logger_(LogManager::getLogger(*this)), connection_(std::move(connection)),
      offer_identity_(std::move(offer_identity))
{
}

bool InitialLoginSessionHandler::handle(const bp::LoginPacket &packet)
{
    constexpr std::size_t salt_size = 16;

    if (profile_) {
        connection_->closeWith(DisconnectFailReason::UnexpectedPacket);
        return true;
    }
    const auto protocol = connection_->getProtocolVersion().getProtocol();
    if (packet.client_network_version != protocol) {
        connection_->write(bp::PlayStatusPacket{.status = packet.client_network_version > protocol
                                                            ? bp::PlayStatus::LoginFailedServerOld
                                                            : bp::PlayStatus::LoginFailedClientOld});
        connection_->close();
        return true;
    }

    const auto request = ConnectionRequest::fromString(packet.connection_request);
    if (!request ||
        (request->authentication_type != 0 && request->authentication_type != ConnectionRequest::SELF_SIGNED)) {
        connection_->closeWith(DisconnectFailReason::NotAuthenticated);
        return true;
    }

    auto *const keys = server_.getMinecraftServiceKeyManager();
    std::optional<GameServerToken> identity;
    if (keys != nullptr) {
        auto validated = GameServerToken::validate(request->token, *keys);
        if (!validated) {
            connection_->closeWith(validated.error());
            return true;
        }
        identity = std::move(*validated);
    }
    else {
        identity = GameServerToken::parse(request->token);
    }
    if (!identity) {
        connection_->closeWith(DisconnectFailReason::NotAuthenticated);
        return true;
    }

    const auto client_key = decodeSubjectPublicKeyInfo(identity->getClientPublicKey());
    const auto self_signed = request->authentication_type == ConnectionRequest::SELF_SIGNED;
    if (!client_key || (self_signed && !request->token.verify(client_key.get())) ||
        request->client_data.x5u != identity->getClientPublicKey() || !request->client_data.verify(client_key.get())) {
        connection_->closeWith(DisconnectFailReason::NotAuthenticated);
        return true;
    }
    if (offer_identity_ && (offer_identity_->getXuid() != identity->getXuid() ||
                            offer_identity_->getClientPublicKey() != identity->getClientPublicKey())) {
        connection_->closeWith(DisconnectFailReason::NotAuthenticated);
        return true;
    }

    ClientDataName client_data;
    std::ignore = glz::read<LENIENT_READ>(client_data, request->client_data.claims);
    auto name = identity->getXuid().empty() ? client_data.third_party_name.value_or("") : identity->getDisplayName();
    if (name.empty()) {
        connection_->closeWith(DisconnectFailReason::InvalidName);
        return true;
    }
    const auto id = identity->getXuid().empty() ? UuidUtils::generateOfflinePlayerUuid(name)
                                                : UuidUtils::generateXuidPlayerUuid(identity->getXuid());
    profile_ = GameProfile{
        .id = id, .xuid = identity->getXuid(), .name = std::move(name), .client_data = request->client_data.claims};

    if (keys == nullptr || connection_->isTransportEncrypted()) {
        finishLogin(std::move(*profile_));
        return true;
    }

    auto *const server_key = connection_->getEndpoint().getIdentity()->getKey();
    std::array<std::byte, salt_size> salt{};
    unsigned char *der = nullptr;
    const auto der_length = i2d_PUBKEY(server_key, &der);
    const std::unique_ptr<unsigned char, decltype([](unsigned char *p) { OPENSSL_free(p); })> owned_der(der);
    if (der_length <= 0 || RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), salt_size) != 1) {
        connection_->closeWith(DisconnectFailReason::NotAuthenticated);
        return true;
    }
    const auto key = deriveEncryptionKey(server_key, client_key.get(), salt);
    if (!key) {
        connection_->closeWith(DisconnectFailReason::NotAuthenticated);
        return true;
    }

    std::string header;
    std::string claims;
    std::ignore = glz::write_json(
        HandshakeHeader{.alg = std::string(curveOf(server_key)->algorithm),
                        .x5u = base64Encode(std::as_bytes(std::span{der, static_cast<std::size_t>(der_length)}))},
        header);
    std::ignore = glz::write_json(HandshakeClaims{.salt = base64Encode(salt)}, claims);
    const auto signing_input = base64UrlEncode(bytesOf(header)) + "." + base64UrlEncode(bytesOf(claims));
    const auto signature = signJws(server_key, signing_input);
    if (!signature) {
        connection_->closeWith(DisconnectFailReason::NotAuthenticated);
        return true;
    }
    connection_->write(bp::ServerToClientHandshakePacket{.token = signing_input + "." + *signature});
    connection_->enableEncryption(*key);
    return true;
}

bool InitialLoginSessionHandler::handle(const bp::ClientToServerHandshakePacket &)
{
    if (!profile_ || server_.getMinecraftServiceKeyManager() == nullptr || connection_->isTransportEncrypted()) {
        connection_->closeWith(DisconnectFailReason::UnexpectedPacket);
        return true;
    }
    finishLogin(std::move(*profile_));
    return true;
}

void InitialLoginSessionHandler::handleUnknown(ByteBufView)
{
    connection_->close();
}

void InitialLoginSessionHandler::finishLogin(GameProfile profile)
{
    connection_->setSessionHandler(std::make_shared<AuthSessionHandler>(server_, connection_, std::move(profile)));
}

AuthSessionHandler::AuthSessionHandler(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection,
                                       GameProfile profile)
    : server_(server), logger_(LogManager::getLogger(*this)), connection_(std::move(connection)),
      profile_(std::move(profile))
{
}

void AuthSessionHandler::activated()
{
    constexpr auto already_connected = "You are already connected to this proxy!";

    const auto player = std::make_shared<Player>(server_, connection_, std::move(profile_));
    if (!server_.canRegisterConnection(*player) || !server_.registerConnection(player)) {
        connection_->closeWith(DisconnectFailReason::NoReason, already_connected);
        return;
    }

    connection_->write(bp::PlayStatusPacket{.status = bp::PlayStatus::LoginSuccess});
    if (server_.getConfiguration().isLogPlayerConnections()) {
        logger_.info("[connected player] {} ({}) has connected", player->getUsername(),
                     connection_->getRemoteAddress());
    }
    connection_->setState(StateRegistry::Play);
    connection_->setSessionHandler(std::make_shared<InitialConnectSessionHandler>(player));
    auto *const target = player->getNextServerToTry();
    if (target == nullptr) {
        connection_->closeWith(DisconnectFailReason::NoReason, "There are no available servers to connect you to. "
                                                               "Try again later or contact an admin.");
        return;
    }
    player->createConnectionRequest(*target).fireAndForget();
}

void AuthSessionHandler::handleUnknown(ByteBufView) {}

}  // namespace endgate
