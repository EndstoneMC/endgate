/**
 * One end of a Bedrock connection, driven over stdin and stdout so a test can put it on any transport.
 *
 * Commands on stdin, one per line:
 *   start                client: send RequestNetworkSettings
 *   wire <hex>           a message that arrived from the transport
 *   text <hex>           send a TextPacket whose message is the hex-decoded bytes
 *   disconnect <hex>     send a DisconnectPacket carrying the hex-decoded message
 *   kick <reason> <hex>  send a DisconnectPacket with this reason and the hex-decoded message
 *   transfer <host> <port>
 *   chunk <x> <z>        send a LevelChunk with no subchunks
 *
 * Events on stdout, one per line:
 *   wire <hex>           a message for the transport
 *   settings <threshold> <algorithm>
 *   handshake            the encryption handshake finished
 *   play-status <status>
 *   login-token <json>   the claims of the login's token
 *   client-data <json>   the claims of the login's client data
 *   text <hex>
 *   disconnect <reason> <hex>
 *   start-game <runtime id> <unique id> <client-side generation> <seed>
 *   camera <fade|clear> [<in> <hold> <out>]
 *   change-dimension <dimension>
 *   chunk <x> <z>
 *   transfer <host> <port>
 *   initialized <runtime id>
 *   loading-screen <type>
 *   chunk-radius <radius>
 *   packet <id>          any other packet
 *   error <what>
 *
 * With --play, a server plays a join after the login: the pack exchange, then StartGame with
 * --runtime-id, --unique-id, --checksum and --dimension, then PlayStatus PlayerSpawn. A client answers the pack
 * exchange and the spawn as the game does.
 *
 * The batch framing, compression and encryption here are written from the wire format, not shared with
 * the proxy, so a mistake the proxy makes on both of its legs shows up as a failure.
 */
#include <libdeflate.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <bedrock/protocol/camera.h>
#include <bedrock/protocol/chunk.h>
#include <bedrock/protocol/dimension.h>
#include <bedrock/protocol/game.h>
#include <bedrock/protocol/network.h>
#include <bedrock/protocol/pack.h>
#include <bedrock/protocol/packet.hpp>
#include <bedrock/protocol/player.h>
#include <bedrock/protocol/presence.h>
#include <bedrock/protocol/serializer.hpp>
#include <bedrock/protocol/stream.hpp>
#include <bedrock/protocol/text.h>
#include <bedrock/protocol/ui.h>
#include <glaze/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include "endgate/crypto/jose.h"
#include "endgate/network/version.h"

namespace bp = bedrock::protocol;

namespace {

constexpr std::uint8_t RAKNET_PREFIX = 0xFE;
constexpr std::uint8_t NO_COMPRESSION = 0xFF;
constexpr std::size_t CHECKSUM_SIZE = 8;
constexpr glz::opts LENIENT_READ{.error_on_unknown_keys = false, .error_on_missing_keys = false};

struct Authentication {
    std::string token;

    struct glaze {
        using T = Authentication;
        static constexpr auto value = glz::object("Token", &T::token);
    };
};

struct Claims {
    std::string cpk;
    std::string salt;

    struct glaze {
        using T = Claims;
        static constexpr auto value = glz::object("cpk", &T::cpk, "salt", &T::salt);
    };
};

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

std::string toHex(std::string_view bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const char c : bytes) {
        out.push_back(digits[(static_cast<unsigned char>(c) >> 4) & 0xF]);
        out.push_back(digits[static_cast<unsigned char>(c) & 0xF]);
    }
    return out;
}

std::string fromHex(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
    }
    return out;
}

void writeVarInt(std::string &out, std::uint32_t value)
{
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

std::uint32_t readVarInt(std::string_view &in)
{
    std::uint32_t value = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (in.empty()) {
            throw std::runtime_error("truncated varint");
        }
        const auto byte = static_cast<std::uint8_t>(in.front());
        in.remove_prefix(1);
        value |= static_cast<std::uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    throw std::runtime_error("varint too long");
}

std::string readLongString(std::string_view &in)
{
    if (in.size() < 4) {
        throw std::runtime_error("truncated string");
    }
    std::uint32_t length = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        length |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i])) << (8 * i);
    }
    in.remove_prefix(4);
    if (length > in.size()) {
        throw std::runtime_error("string runs past the end");
    }
    std::string out(in.substr(0, length));
    in.remove_prefix(length);
    return out;
}

std::span<const std::byte> bytesOf(std::string_view text)
{
    return std::as_bytes(std::span{text.data(), text.size()});
}

std::string stringOf(std::span<const std::byte> bytes)
{
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

std::string sha256(std::initializer_list<std::string_view> parts)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr);
    for (const auto part : parts) {
        EVP_DigestUpdate(context.get(), part.data(), part.size());
    }
    EVP_DigestFinal_ex(context.get(), digest.data(), &length);
    return {reinterpret_cast<const char *>(digest.data()), length};
}

std::string sharedSecret(EVP_PKEY *own, EVP_PKEY *peer)
{
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new(own, nullptr),
                                                                              EVP_PKEY_CTX_free);
    std::size_t size = 0;
    if (EVP_PKEY_derive_init(context.get()) != 1 || EVP_PKEY_derive_set_peer(context.get(), peer) != 1 ||
        EVP_PKEY_derive(context.get(), nullptr, &size) != 1) {
        throw std::runtime_error("key agreement failed");
    }
    std::string secret(size, '\0');
    if (EVP_PKEY_derive(context.get(), reinterpret_cast<unsigned char *>(secret.data()), &size) != 1) {
        throw std::runtime_error("key agreement failed");
    }
    secret.resize(size);
    return secret;
}

/**
 * One direction of Bedrock's batch encryption: AES-256 in counter mode from the key and an IV of its
 * first 12 bytes then 00 00 00 02, never reset, with the first 8 bytes of SHA-256(counter, batch, key)
 * after each batch.
 */
class Cipher {
public:
    Cipher(std::string key, bool encrypt) : key_(std::move(key)), context_(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free)
    {
        std::string iv = key_.substr(0, 12);
        iv.append({0, 0, 0, 2});
        const auto *const raw_key = reinterpret_cast<const unsigned char *>(key_.data());
        const auto *const raw_iv = reinterpret_cast<const unsigned char *>(iv.data());
        const auto ready = encrypt ? EVP_EncryptInit_ex(context_.get(), EVP_aes_256_ctr(), nullptr, raw_key, raw_iv)
                                   : EVP_DecryptInit_ex(context_.get(), EVP_aes_256_ctr(), nullptr, raw_key, raw_iv);
        if (ready != 1) {
            throw std::runtime_error("cipher setup failed");
        }
    }

    std::string seal(std::string_view batch)
    {
        std::string plain(batch);
        plain += checksum(batch);
        std::string out(plain.size(), '\0');
        int produced = 0;
        EVP_EncryptUpdate(context_.get(), reinterpret_cast<unsigned char *>(out.data()), &produced,
                          reinterpret_cast<const unsigned char *>(plain.data()), static_cast<int>(plain.size()));
        ++counter_;
        return out;
    }

    std::string open(std::string_view sealed)
    {
        std::string plain(sealed.size(), '\0');
        int produced = 0;
        EVP_DecryptUpdate(context_.get(), reinterpret_cast<unsigned char *>(plain.data()), &produced,
                          reinterpret_cast<const unsigned char *>(sealed.data()), static_cast<int>(sealed.size()));
        if (plain.size() < CHECKSUM_SIZE) {
            throw std::runtime_error("batch too short for its checksum");
        }
        const auto sum = plain.substr(plain.size() - CHECKSUM_SIZE);
        plain.resize(plain.size() - CHECKSUM_SIZE);
        if (sum != checksum(plain)) {
            throw std::runtime_error("batch checksum mismatch");
        }
        ++counter_;
        return plain;
    }

private:
    [[nodiscard]] std::string checksum(std::string_view batch) const
    {
        std::string counter;
        for (std::size_t i = 0; i < 8; ++i) {
            counter.push_back(static_cast<char>(counter_ >> (8 * i)));
        }
        return sha256({counter, batch, key_}).substr(0, CHECKSUM_SIZE);
    }

    std::string key_;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context_;
    std::uint64_t counter_{0};
};

struct Options {
    bool server{false};
    bool prefix{false};
    bool encrypt{false};
    int protocol = endgate::MAXIMUM_VERSION.getProtocol();
    std::string request;
    std::string key_path;
    std::uint16_t threshold{0};
    int delay_ms{0};
    bool play = false;
    std::uint64_t runtime_id = 1;
    std::int64_t unique_id = -1;
    std::uint64_t checksum = 0;
    int dimension = 0;
};

class Endpoint {
public:
    Endpoint() = default;
    Endpoint(const Endpoint &) = delete;
    Endpoint &operator=(const Endpoint &) = delete;
    virtual ~Endpoint() = default;

    virtual void start() = 0;
    virtual void sendText(std::string message) = 0;
    virtual void sendDisconnect(const std::string &message) = 0;
    virtual void sendKick(int reason, const std::string &message) = 0;
    virtual void sendTransfer(std::string host, std::uint16_t port) = 0;
    virtual void sendChunk(std::int32_t x, std::int32_t z) = 0;
    virtual void receive(std::string_view message) = 0;
};

template <int V>
class Peer final : public Endpoint {
public:
    explicit Peer(Options options) : options_(std::move(options))
    {
        if (options_.server) {
            key_.reset(EVP_EC_gen("P-384"));
        }
        else if (!options_.key_path.empty()) {
            const std::unique_ptr<FILE, decltype(&fclose)> file(fopen(options_.key_path.c_str(), "r"), fclose);
            key_.reset(PEM_read_PrivateKey(file.get(), nullptr, nullptr, nullptr));
            if (!key_) {
                throw std::runtime_error("cannot read the client key");
            }
        }
    }

    void start() override { send(bp::RequestNetworkSettingsPacket{.client_network_version = options_.protocol}); }

    void sendText(std::string message) override
    {
        bp::TextPacket_<V> packet;
        packet.body =
            typename bp::TextPacket_<V>::MessageOnly{.type = bp::TextPacketType::Raw, .message = std::move(message)};
        send(packet);
    }

    void sendDisconnect(const std::string &message) override
    {
        bp::DisconnectPacket_<V> packet;
        packet.reason = {};
        packet.messages = bp::DisconnectPacketMessages{.message = message, .filtered_message = message};
        send(packet);
    }

    void sendKick(int reason, const std::string &message) override
    {
        bp::DisconnectPacket_<V> packet;
        packet.reason = static_cast<decltype(packet.reason)>(reason);
        packet.messages = bp::DisconnectPacketMessages{.message = message, .filtered_message = message};
        send(packet);
    }

    void sendTransfer(std::string host, std::uint16_t port) override
    {
        bp::TransferPacket_<V> packet;
        packet.destination = std::move(host);
        packet.destination_port = port;
        send(packet);
    }

    void sendChunk(std::int32_t x, std::int32_t z) override
    {
        bp::LevelChunkPacket_<V> packet;
        packet.pos = {.x = x, .z = z};
        packet.serialized_chunk = std::string(1, '\0');
        send(packet);
    }

    void receive(std::string_view message) override
    {
        if (options_.prefix) {
            if (message.empty() || static_cast<std::uint8_t>(message.front()) != RAKNET_PREFIX) {
                throw std::runtime_error("message without the RakNet prefix");
            }
            message.remove_prefix(1);
        }
        std::string batch(message);
        if (decrypt_) {
            batch = decrypt_->open(batch);
        }
        if (compressing_) {
            batch = decompress(batch);
        }
        std::string_view frames = batch;
        while (!frames.empty()) {
            const auto length = readVarInt(frames);
            if (length == 0 || length > frames.size()) {
                throw std::runtime_error("bad frame length");
            }
            auto packet = frames.substr(0, length);
            frames.remove_prefix(length);
            dispatch(packet);
        }
    }

private:
    template <typename Packet>
    static Packet decode(std::string_view body)
    {
        bp::BinaryReader reader{body};
        auto packet = bp::deserialize<Packet>(reader);
        if (!packet || reader.getUnreadLength() != 0) {
            throw std::runtime_error("packet does not decode");
        }
        return std::move(*packet);
    }

    template <typename Packet>
    void send(const Packet &packet)
    {
        std::string body;
        bp::BinaryWriter writer{body};
        bp::serialize(writer, packet);
        std::string frame;
        writeVarInt(frame, Packet::Id);
        frame += body;
        std::string batch;
        writeVarInt(batch, static_cast<std::uint32_t>(frame.size()));
        batch += frame;
        if (compressing_) {
            batch = compress(batch);
        }
        if (encrypt_) {
            batch = encrypt_->seal(batch);
        }
        if (options_.prefix) {
            batch.insert(batch.begin(), static_cast<char>(RAKNET_PREFIX));
        }
        std::cout << "wire " << toHex(batch) << '\n';
    }

    [[nodiscard]] std::string compress(const std::string &batch) const
    {
        if (batch.size() < options_.threshold) {
            return std::string(1, static_cast<char>(NO_COMPRESSION)) + batch;
        }
        const std::unique_ptr<libdeflate_compressor, decltype(&libdeflate_free_compressor)> compressor(
            libdeflate_alloc_compressor(6), libdeflate_free_compressor);
        std::string out(1 + libdeflate_deflate_compress_bound(compressor.get(), batch.size()), '\0');
        const auto produced =
            libdeflate_deflate_compress(compressor.get(), batch.data(), batch.size(), out.data() + 1, out.size() - 1);
        out.resize(1 + produced);
        out[0] = 0;
        return out;
    }

    static std::string decompress(std::string_view batch)
    {
        if (batch.empty()) {
            throw std::runtime_error("empty batch");
        }
        const auto header = static_cast<std::uint8_t>(batch.front());
        batch.remove_prefix(1);
        if (header == NO_COMPRESSION) {
            return std::string(batch);
        }
        if (header != 0) {
            throw std::runtime_error("unsupported compression");
        }
        const std::unique_ptr<libdeflate_decompressor, decltype(&libdeflate_free_decompressor)> decompressor(
            libdeflate_alloc_decompressor(), libdeflate_free_decompressor);
        for (std::size_t capacity = std::max<std::size_t>(batch.size() * 8, 4096); capacity <= (64U << 20);
             capacity *= 2) {
            std::string out(capacity, '\0');
            std::size_t produced = 0;
            const auto result = libdeflate_deflate_decompress(decompressor.get(), batch.data(), batch.size(),
                                                              out.data(), capacity, &produced);
            if (result == LIBDEFLATE_SUCCESS) {
                out.resize(produced);
                return out;
            }
            if (result != LIBDEFLATE_INSUFFICIENT_SPACE) {
                throw std::runtime_error("corrupt compressed batch");
            }
        }
        throw std::runtime_error("batch too large");
    }

    void enableEncryption(const std::string &salt, EVP_PKEY *peer)
    {
        const auto key = sha256({salt, sharedSecret(key_.get(), peer)});
        encrypt_.emplace(key, true);
        decrypt_.emplace(key, false);
    }

    void dispatch(std::string_view packet)
    {
        const auto id = static_cast<int>(readVarInt(packet) & 0x3FF);
        if (id == bp::TextPacket_<V>::Id) {
            const auto text = decode<bp::TextPacket_<V>>(packet);
            std::cout << "text " << toHex(std::visit([](const auto &body) { return body.message; }, text.body)) << '\n';
        }
        else if (options_.server) {
            dispatchServerbound(id, packet);
        }
        else {
            dispatchClientbound(id, packet);
        }
    }

    void dispatchServerbound(int id, std::string_view packet)
    {
        if (id == bp::RequestNetworkSettingsPacket::Id) {
            const auto request = decode<bp::RequestNetworkSettingsPacket>(packet);
            if (request.client_network_version != options_.protocol) {
                send(bp::PlayStatusPacket{.status = bp::PlayStatus::LoginFailedClientOld});
                return;
            }
            send(bp::NetworkSettingsPacket{.compression_threshold = options_.threshold,
                                           .compression_algorithm = bp::PacketCompressionAlgorithm::ZLib});
            compressing_ = true;
        }
        else if (id == bp::LoginPacket::Id) {
            onLogin(decode<bp::LoginPacket>(packet));
        }
        else if (id == bp::ClientToServerHandshakePacket::Id) {
            std::cout << "handshake\n";
            loggedIn();
        }
        else if (id == bp::ResourcePackClientResponsePacket_<V>::Id) {
            const auto response = decode<bp::ResourcePackClientResponsePacket_<V>>(packet).response;
            if (std::holds_alternative<typename bp::ResourcePackClientResponsePacket_<V>::DownloadingFinished>(
                    response)) {
                send(bp::ResourcePackStackPacket_<V>{});
            }
            else if (std::holds_alternative<
                         typename bp::ResourcePackClientResponsePacket_<V>::ResourcePackStackFinished>(response)) {
                bp::StartGamePacket_<V> start_game;
                start_game.runtime_id = bp::ActorRuntimeID{options_.runtime_id};
                start_game.entity_id = bp::ActorUniqueID{options_.unique_id};
                start_game.server_block_type_registry_checksum = options_.checksum;
                start_game.settings.spawn_settings.dimension =
                    static_cast<decltype(start_game.settings.spawn_settings.dimension)>(options_.dimension);
                send(start_game);
                send(bp::PlayStatusPacket{.status = bp::PlayStatus::PlayerSpawn});
            }
        }
        else if (id == bp::SetLocalPlayerAsInitializedPacket::Id) {
            std::cout << "initialized "
                      << static_cast<std::uint64_t>(decode<bp::SetLocalPlayerAsInitializedPacket>(packet).player_id)
                      << '\n';
        }
        else if (id == bp::ServerboundLoadingScreenPacket::Id) {
            std::cout << "loading-screen "
                      << static_cast<int>(decode<bp::ServerboundLoadingScreenPacket>(packet).loading_screen_packet_type)
                      << '\n';
        }
        else if (id == bp::RequestChunkRadiusPacket::Id) {
            std::cout << "chunk-radius " << decode<bp::RequestChunkRadiusPacket>(packet).chunk_radius << '\n';
        }
        else if (options_.play) {
            std::cout << "packet " << id << '\n';
        }
        else {
            std::cout << "error unexpected packet " << id << '\n';
        }
    }

    void dispatchClientbound(int id, std::string_view packet)
    {
        if (id == bp::NetworkSettingsPacket::Id) {
            const auto settings = decode<bp::NetworkSettingsPacket>(packet);
            std::cout << "settings " << settings.compression_threshold << ' '
                      << static_cast<int>(settings.compression_algorithm) << '\n';
            options_.threshold = settings.compression_threshold;
            compressing_ = true;
            send(bp::LoginPacket{.client_network_version = options_.protocol,
                                 .connection_request = fromHex(options_.request)});
        }
        else if (id == bp::ServerToClientHandshakePacket::Id) {
            const auto handshake = decode<bp::ServerToClientHandshakePacket>(packet);
            const auto token = endgate::JsonWebToken::decode(handshake.token);
            const auto server_key = token ? endgate::decodeSubjectPublicKeyInfo(token->x5u) : nullptr;
            Claims claims;
            if (!server_key || glz::read<LENIENT_READ>(claims, token->claims) || !endgate::base64Decode(claims.salt)) {
                throw std::runtime_error("bad handshake token");
            }
            enableEncryption(stringOf(*endgate::base64Decode(claims.salt)), server_key.get());
            send(bp::ClientToServerHandshakePacket{});
            std::cout << "handshake\n";
        }
        else if (id == bp::PlayStatusPacket::Id) {
            const auto status = decode<bp::PlayStatusPacket>(packet).status;
            std::cout << "play-status " << static_cast<int>(status) << '\n';
            if (status == bp::PlayStatus::PlayerSpawn && runtime_id_ && !initialized_) {
                initialized_ = true;
                send(bp::ServerboundLoadingScreenPacket{.loading_screen_packet_type =
                                                            bp::ServerboundLoadingScreenPacketType::EndLoadingScreen});
                send(bp::SetLocalPlayerAsInitializedPacket{.player_id = *runtime_id_});
            }
        }
        else if (id == bp::ResourcePacksInfoPacket_<V>::Id) {
            send(bp::ResourcePackClientResponsePacket_<V>{
                .response = typename bp::ResourcePackClientResponsePacket_<V>::DownloadingFinished{}});
        }
        else if (id == bp::ResourcePackStackPacket_<V>::Id) {
            send(bp::ResourcePackClientResponsePacket_<V>{
                .response = typename bp::ResourcePackClientResponsePacket_<V>::ResourcePackStackFinished{}});
        }
        else if (id == bp::StartGamePacket_<V>::Id) {
            const auto start_game = decode<bp::StartGamePacket_<V>>(packet);
            std::cout << "start-game " << static_cast<std::uint64_t>(start_game.runtime_id) << ' '
                      << static_cast<std::int64_t>(start_game.entity_id) << ' '
                      << start_game.server_enabled_client_side_generation << ' ' << start_game.settings.seed << '\n';
            runtime_id_ = start_game.runtime_id;
            send(bp::RequestChunkRadiusPacket{.chunk_radius = 8, .max_chunk_radius = 12});
            send(bp::ServerboundLoadingScreenPacket{.loading_screen_packet_type =
                                                        bp::ServerboundLoadingScreenPacketType::StartLoadingScreen});
        }
        else if (id == bp::CameraInstructionPacket_<V>::Id) {
            const auto instruction = decode<bp::CameraInstructionPacket_<V>>(packet).camera_instruction;
            if (instruction.fade && instruction.fade->time) {
                std::cout << "camera fade " << instruction.fade->time->fade_in_time << ' '
                          << instruction.fade->time->hold_time << ' ' << instruction.fade->time->fade_out_time << '\n';
            }
            else {
                std::cout << "camera clear\n";
            }
        }
        else if (id == bp::ChangeDimensionPacket::Id) {
            std::cout << "change-dimension " << static_cast<int>(decode<bp::ChangeDimensionPacket>(packet).dimension_id)
                      << '\n';
        }
        else if (id == bp::LevelChunkPacket_<V>::Id) {
            const auto chunk = decode<bp::LevelChunkPacket_<V>>(packet);
            std::cout << "chunk " << chunk.pos.x << ' ' << chunk.pos.z << '\n';
        }
        else if (id == bp::TransferPacket_<V>::Id) {
            const auto transfer = decode<bp::TransferPacket_<V>>(packet);
            std::cout << "transfer " << transfer.destination << ' ' << transfer.destination_port << '\n';
        }
        else if (id == bp::DisconnectPacket_<V>::Id) {
            const auto disconnect = decode<bp::DisconnectPacket_<V>>(packet);
            const auto *const messages = std::get_if<bp::DisconnectPacketMessages>(&disconnect.messages);
            std::cout << "disconnect " << static_cast<int>(disconnect.reason) << ' '
                      << toHex(messages != nullptr ? messages->message : "") << '\n';
        }
        else {
            std::cout << "packet " << id << '\n';
        }
    }

    void loggedIn()
    {
        send(bp::PlayStatusPacket{.status = bp::PlayStatus::LoginSuccess});
        if (options_.play) {
            send(bp::ResourcePacksInfoPacket_<V>{});
        }
    }

    void onLogin(const bp::LoginPacket &login)
    {
        std::string_view request = login.connection_request;
        const auto authentication = readLongString(request);
        const auto client_data = endgate::JsonWebToken::decode(readLongString(request), 1U << 20);
        Authentication parsed;
        const auto token = glz::read<LENIENT_READ>(parsed, authentication)
                             ? std::nullopt
                             : endgate::JsonWebToken::decode(parsed.token);
        if (!token || !client_data) {
            throw std::runtime_error("login does not parse");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options_.delay_ms));
        std::cout << "login-token " << token->claims << '\n';
        std::cout << "client-data " << client_data->claims << '\n';
        Claims claims;
        if (glz::read<LENIENT_READ>(claims, token->claims)) {
            throw std::runtime_error("token claims do not parse");
        }
        if (!options_.encrypt) {
            loggedIn();
            return;
        }
        const auto client_key = endgate::decodeSubjectPublicKeyInfo(claims.cpk);
        unsigned char *der = nullptr;
        const auto der_length = i2d_PUBKEY(key_.get(), &der);
        const std::unique_ptr<unsigned char, decltype([](unsigned char *p) { OPENSSL_free(p); })> owned_der(der);
        std::string salt(16, '\0');
        RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), 16);
        const auto header =
            std::format(R"({{"alg":"ES384","x5u":"{}"}})",
                        endgate::base64Encode(std::as_bytes(std::span{der, static_cast<std::size_t>(der_length)})));
        const auto body = std::format(R"({{"salt":"{}"}})", endgate::base64Encode(bytesOf(salt)));
        const auto signing_input =
            endgate::base64UrlEncode(bytesOf(header)) + "." + endgate::base64UrlEncode(bytesOf(body));
        const auto signature = endgate::signJws(key_.get(), signing_input);
        if (!client_key || !signature) {
            throw std::runtime_error("cannot build the handshake");
        }
        send(bp::ServerToClientHandshakePacket{.token = signing_input + "." + *signature});
        enableEncryption(salt, client_key.get());
    }

    Options options_;
    Key key_{nullptr, EVP_PKEY_free};
    bool compressing_{false};
    std::optional<Cipher> encrypt_;
    std::optional<Cipher> decrypt_;
    std::optional<bp::ActorRuntimeID> runtime_id_;
    bool initialized_ = false;
};

std::unique_ptr<Endpoint> makePeer(const Options &options)
{
    std::unique_ptr<Endpoint> peer;
    const auto supported = endgate::visitProtocolVersion(options.protocol, [&]<int V>() {
        peer = std::make_unique<Peer<V>>(options);
        return true;
    });
    if (!supported) {
        peer = std::make_unique<Peer<endgate::MAXIMUM_VERSION.getProtocol()>>(options);
    }
    return peer;
}

}  // namespace

int main(int argc, char **argv)
{
    std::ios::sync_with_stdio(false);
    std::cout << std::unitbuf;
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto value = [&] {
            return std::string(argv[++i]);
        };
        if (argument == "server") {
            options.server = true;
        }
        else if (argument == "--raknet") {
            options.prefix = true;
        }
        else if (argument == "--encrypt") {
            options.encrypt = true;
        }
        else if (argument == "--protocol") {
            options.protocol = std::stoi(value());
        }
        else if (argument == "--threshold") {
            options.threshold = static_cast<std::uint16_t>(std::stoi(value()));
        }
        else if (argument == "--delay") {
            options.delay_ms = std::stoi(value());
        }
        else if (argument == "--key") {
            options.key_path = value();
        }
        else if (argument == "--request") {
            options.request = value();
        }
        else if (argument == "--play") {
            options.play = true;
        }
        else if (argument == "--runtime-id") {
            options.runtime_id = std::stoull(value());
        }
        else if (argument == "--unique-id") {
            options.unique_id = std::stoll(value());
        }
        else if (argument == "--checksum") {
            options.checksum = std::stoull(value());
        }
        else if (argument == "--dimension") {
            options.dimension = std::stoi(value());
        }
    }

    const auto peer_ptr = makePeer(options);
    Endpoint &peer = *peer_ptr;
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto space = line.find(' ');
        const auto command = line.substr(0, space);
        const auto argument = space == std::string::npos ? std::string{} : line.substr(space + 1);
        try {
            if (command == "start") {
                peer.start();
            }
            else if (command == "wire") {
                peer.receive(fromHex(argument));
            }
            else if (command == "text") {
                peer.sendText(fromHex(argument));
            }
            else if (command == "disconnect") {
                peer.sendDisconnect(fromHex(argument));
            }
            else if (command == "kick") {
                const auto space = argument.find(' ');
                peer.sendKick(std::stoi(argument.substr(0, space)), fromHex(argument.substr(space + 1)));
            }
            else if (command == "transfer") {
                const auto space = argument.find(' ');
                peer.sendTransfer(argument.substr(0, space),
                                  static_cast<std::uint16_t>(std::stoi(argument.substr(space + 1))));
            }
            else if (command == "chunk") {
                const auto space = argument.find(' ');
                peer.sendChunk(std::stoi(argument.substr(0, space)), std::stoi(argument.substr(space + 1)));
            }
        }
        catch (const std::exception &error) {
            std::cout << "error " << error.what() << '\n';
        }
    }
}
