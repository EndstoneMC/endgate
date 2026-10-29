#include "endgate/network/nethernet/client.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <format>
#include <ranges>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/cancel_after.hpp>
#include <asio/connect.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <glaze/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#include "endgate/awaitable.h"
#include "endgate/network/nethernet/identity.h"

namespace endgate::nethernet {

namespace {

constexpr auto AWAITABLE = asio::as_tuple(asio::use_awaitable);

struct HttpResponse {
    std::uint16_t status{0};
    std::string content_type;
    std::string body;
};

struct Capability {
    std::string name;
    int protocol{0};
    std::string version;
    std::string level;
    int players{0};
    int max_players{0};
    int game_type{0};

    struct glaze {
        using T = Capability;
        static constexpr auto value =
            glz::object("name", &T::name, "protocol", &T::protocol, "version", &T::version, "level", &T::level,
                        "players", &T::players, "maxPlayers", &T::max_players, "gameType", &T::game_type);
    };
};

template <typename Stream>
Awaitable<std::optional<HttpResponse>> readExchange(Stream &stream, const std::string &request,
                                                    const SignalingClient::Config &config)
{
    const auto [write_error, written] = co_await asio::async_write(
        stream, asio::buffer(request), asio::cancel_after(config.request_timeout, AWAITABLE));
    if (write_error) {
        co_return std::nullopt;
    }

    std::string buffer;
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        std::array<char, 4096> chunk{};
        const auto [error, received] =
            co_await stream.async_read_some(asio::buffer(chunk), asio::cancel_after(config.request_timeout, AWAITABLE));
        if (error || buffer.size() + received > config.max_response_bytes) {
            co_return std::nullopt;
        }
        buffer.append(chunk.data(), received);
        header_end = buffer.find("\r\n\r\n");
    }

    HttpResponse response;
    std::string_view head(buffer.data(), header_end + 2);
    const auto status_line = head.substr(0, head.find("\r\n"));
    const auto first_space = status_line.find(' ');
    if (first_space == std::string_view::npos) {
        co_return std::nullopt;
    }
    const auto status_text = status_line.substr(first_space + 1, 3);
    if (std::from_chars(status_text.data(), status_text.data() + status_text.size(), response.status).ec !=
        std::errc{}) {
        co_return std::nullopt;
    }
    head.remove_prefix(status_line.size() + 2);

    std::optional<std::size_t> content_length;
    while (!head.empty()) {
        const auto header = head.substr(0, head.find("\r\n"));
        head.remove_prefix(std::min(head.size(), header.size() + 2));
        const auto colon = header.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const auto name = header.substr(0, colon);
        auto value = header.substr(colon + 1);
        value.remove_prefix(std::min(value.find_first_not_of(" \t"), value.size()));
        value.remove_suffix(value.size() - std::min(value.find_last_not_of(" \t") + 1, value.size()));
        const auto equal = [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
        };
        if (std::ranges::equal(name, std::string_view{"content-length"}, equal)) {
            std::size_t length = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), length).ec == std::errc{}) {
                content_length = length;
            }
        }
        else if (std::ranges::equal(name, std::string_view{"content-type"}, equal)) {
            response.content_type = value;
        }
    }

    const auto body_start = header_end + 4;
    if (content_length) {
        if (*content_length > config.max_response_bytes) {
            co_return std::nullopt;
        }
        while (buffer.size() < body_start + *content_length) {
            std::array<char, 4096> chunk{};
            const auto [error, received] = co_await stream.async_read_some(
                asio::buffer(chunk), asio::cancel_after(config.request_timeout, AWAITABLE));
            if (error || buffer.size() + received > config.max_response_bytes) {
                co_return std::nullopt;
            }
            buffer.append(chunk.data(), received);
        }
        response.body = buffer.substr(body_start, *content_length);
        co_return response;
    }

    while (buffer.size() <= config.max_response_bytes) {
        std::array<char, 4096> chunk{};
        const auto [error, received] =
            co_await stream.async_read_some(asio::buffer(chunk), asio::cancel_after(config.request_timeout, AWAITABLE));
        buffer.append(chunk.data(), received);
        if (error) {
            break;
        }
    }
    response.body = buffer.substr(body_start);
    co_return response;
}

Awaitable<std::optional<HttpResponse>> httpExchange(asio::io_context &io_context, const std::string &host,
                                                    std::uint16_t port, bool tls, const std::string &request,
                                                    const SignalingClient::Config &config)
{
    asio::ip::tcp::resolver resolver(io_context);
    const auto [resolve_error, endpoints] = co_await resolver.async_resolve(host, std::to_string(port), AWAITABLE);
    if (resolve_error || endpoints.empty()) {
        co_return std::nullopt;
    }
    asio::ip::tcp::socket socket(io_context);
    const auto [connect_error, endpoint] =
        co_await asio::async_connect(socket, endpoints, asio::cancel_after(config.connect_timeout, AWAITABLE));
    if (connect_error) {
        co_return std::nullopt;
    }
    asio::error_code ignored;
    std::ignore = socket.set_option(asio::ip::tcp::no_delay(true), ignored);

    if (!tls) {
        co_return co_await readExchange(socket, request, config);
    }

    auto context = config.tls;
    if (!context) {
        context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
        context->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                             asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                             asio::ssl::context::no_tlsv1_1);
        context->set_verify_mode(asio::ssl::verify_none);
    }
    asio::ssl::stream<asio::ip::tcp::socket &> stream(socket, *context);
    std::error_code address_error;
    std::ignore = asio::ip::make_address(host, address_error);
    if (address_error) {
        SSL_set_tlsext_host_name(stream.native_handle(), host.c_str());
    }
    const auto [handshake_error] = co_await stream.async_handshake(
        asio::ssl::stream_base::client, asio::cancel_after(config.tls_handshake_timeout, AWAITABLE));
    if (handshake_error) {
        co_return std::nullopt;
    }
    auto response = co_await readExchange(stream, request, config);
    std::ignore = co_await stream.async_shutdown(asio::cancel_after(std::chrono::seconds{1}, AWAITABLE));
    co_return response;
}

std::optional<ServerPing> parseCapability(const std::string &body)
{
    Capability capability;
    constexpr glz::opts options{.error_on_unknown_keys = false};
    if (glz::read<options>(capability, body)) {
        return std::nullopt;
    }
    return ServerPing{ServerPing::Version{capability.protocol, capability.version},
                      ServerPing::Players{capability.players, capability.max_players}, capability.name,
                      capability.level, static_cast<GameType>(capability.game_type)};
}

std::string cpkDigest(EVP_PKEY *key)
{
    unsigned char *der = nullptr;
    const auto length = i2d_PUBKEY(key, &der);
    const std::unique_ptr<unsigned char, decltype([](unsigned char *p) { OPENSSL_free(p); })> owned(der);
    if (length <= 0) {
        return {};
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(der, static_cast<std::size_t>(length), digest.data());
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        hex += std::format("{:02x}", byte);
    }
    return hex;
}

std::expected<SignalingClient::Answer, ESessionError> parseAnswer(const std::string &body)
{
    const auto begin = body.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::unexpected(ESessionError::SignalingParsingFailure);
    }
    const auto end = body.find_last_not_of(" \t\r\n");
    const std::string_view trimmed(body.data() + begin, end - begin + 1);

    if (trimmed.starts_with("v=")) {
        std::optional<SignalingClient::ServerAssertion> identity;
        if (const auto assertion = IdentityAssertion::extractFromSdp(body)) {
            if (const auto token = assertion->getToken()) {
                if (const auto jwt = JsonWebToken::decode(*token)) {
                    if (const auto key = jwt->publicKey()) {
                        identity = SignalingClient::ServerAssertion{
                            .idp_domain = assertion->getIdpDomain(),
                            .cpk_digest = cpkDigest(key.get()),
                            .verified = jwt->verify(key.get()) && assertion->verifyFingerprints(key.get())};
                    }
                }
            }
        }
        return SignalingClient::Answer{.sdp = IdentityAssertion::stripFromSdp(body), .identity = std::move(identity)};
    }

    if (std::ranges::all_of(trimmed, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        std::uint32_t code = 0;
        std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), code);
        return std::unexpected(static_cast<ESessionError>(code));
    }
    return std::unexpected(ESessionError::SignalingParsingFailure);
}

}  // namespace

Awaitable<SignalingClient::Probe> SignalingClient::probeNetherNet(asio::io_context &io_context, std::string host,
                                                                  std::uint16_t port, Config config)
{
    const auto request =
        std::format("GET /v1/join HTTP/1.1\r\nHost: {}:{}\r\nUser-Agent: libhttpclient/1.0.0.0\r\nAccept: "
                    "*/*\r\nConnection: Keep-Alive\r\n\r\n",
                    host, port);
    for (const auto tls : {true, false}) {
        const auto response = co_await httpExchange(io_context, host, port, tls, request, config);
        if (!response) {
            if (tls) {
                continue;
            }
            co_return Probe{};
        }
        const auto supported = response->status / 100 == 2;
        co_return Probe{.supported = supported, .ping = supported ? parseCapability(response->body) : std::nullopt};
    }
    co_return Probe{};
}

Awaitable<std::expected<SignalingClient::Answer, ESessionError>> SignalingClient::requestAnswer(
    asio::io_context &io_context, std::string host, std::uint16_t port, std::string network_id, std::string offer,
    Config config)
{
    const auto capability_request =
        std::format("GET /v1/join HTTP/1.1\r\nHost: {}:{}\r\nUser-Agent: libhttpclient/1.0.0.0\r\nAccept: "
                    "*/*\r\nConnection: Keep-Alive\r\n\r\n",
                    host, port);
    const auto join_request =
        std::format("POST /v1/join/{} HTTP/1.1\r\nHost: {}:{}\r\nUser-Agent: libhttpclient/1.0.0.0\r\nContent-Type: "
                    "application/sdp\r\nConnection: Keep-Alive\r\nContent-Length: {}\r\n\r\n{}",
                    network_id, host, port, offer.size(), offer);

    for (const auto tls : {true, false}) {
        const auto capability = co_await httpExchange(io_context, host, port, tls, capability_request, config);
        if (!capability) {
            if (tls) {
                continue;
            }
            co_return std::unexpected(ESessionError::NoSignalingChannel);
        }
        if (capability->status / 100 != 2) {
            co_return std::unexpected(ESessionError::NoSignalingChannel);
        }
        const auto response = co_await httpExchange(io_context, host, port, tls, join_request, config);
        if (!response || response->status / 100 != 2) {
            co_return std::unexpected(ESessionError::SignalingFailedToSend);
        }
        co_return parseAnswer(response->body);
    }
    co_return std::unexpected(ESessionError::NoSignalingChannel);
}

}  // namespace endgate::nethernet
