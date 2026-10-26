#include "endgate/network/nethernet/signaling.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <ranges>
#include <string_view>
#include <tuple>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/cancel_after.hpp>
#include <asio/co_spawn.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/post.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <glaze/json.hpp>

namespace endgate::nethernet {

namespace {

constexpr auto AWAITABLE = asio::as_tuple(asio::use_awaitable);

struct Ping {
    std::string name;
    int protocol;
    std::string version;
    std::string level;
    int players;
    int max_players;
    int game_type;

    struct glaze {
        using T = Ping;
        static constexpr auto value =
            glz::object("name", &T::name, "protocol", &T::protocol, "version", &T::version, "level", &T::level,
                        "players", &T::players, "maxPlayers", &T::max_players, "gameType", &T::game_type);
    };
};

class Registration {
public:
    Registration(std::unordered_set<asio::ip::tcp::socket *> &connections, asio::ip::tcp::socket &socket)
        : connections_(connections), socket_(socket)
    {
        connections_.insert(&socket_);
    }
    Registration(const Registration &) = delete;
    Registration &operator=(const Registration &) = delete;
    Registration(Registration &&) = delete;
    Registration &operator=(Registration &&) = delete;
    ~Registration()
    {
        connections_.erase(&socket_);
        asio::error_code ignored;
        std::ignore = socket_.shutdown(asio::socket_base::shutdown_both, ignored);
        std::ignore = socket_.close(ignored);
    }

private:
    std::unordered_set<asio::ip::tcp::socket *> &connections_;
    asio::ip::tcp::socket &socket_;
};

std::chrono::milliseconds remainingUntil(std::chrono::steady_clock::time_point deadline)
{
    return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()),
                    std::chrono::milliseconds{0});
}

}  // namespace

void JoinResponse::answer(std::string sdp) const
{
    asio::post(pending_->timer.get_executor(), [pending = pending_, sdp = std::move(sdp)]() mutable {
        if (!pending->result) {
            pending->result = std::move(sdp);
            pending->timer.cancel();
        }
    });
}

void JoinResponse::reject(std::uint32_t session_error) const
{
    asio::post(pending_->timer.get_executor(), [pending = pending_, session_error] {
        if (!pending->result) {
            pending->result = session_error;
            pending->timer.cancel();
        }
    });
}

HttpSignalingServer::HttpSignalingServer(asio::io_context &io_context, JoinHandler &handler, Config config,
                                         std::shared_ptr<asio::ssl::context> tls)
    : strand_(asio::make_strand(io_context)), acceptor_(strand_), handler_(handler), config_(config),
      tls_(std::move(tls))
{
}

std::shared_ptr<HttpSignalingServer> HttpSignalingServer::create(asio::io_context &io_context, JoinHandler &handler,
                                                                 Config config, std::shared_ptr<asio::ssl::context> tls)
{
    return std::shared_ptr<HttpSignalingServer>(new HttpSignalingServer(io_context, handler, config, std::move(tls)));
}

std::shared_ptr<asio::ssl::context> HttpSignalingServer::loadTls(const std::filesystem::path &certificate_chain,
                                                                 const std::filesystem::path &private_key)
{
    auto context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
    context->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                         asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
    context->use_certificate_chain_file(certificate_chain.string());
    context->use_private_key_file(private_key.string(), asio::ssl::context::pem);
    return context;
}

void HttpSignalingServer::listen(const asio::ip::tcp::endpoint &endpoint)
{
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    if (endpoint.address().is_v6() && endpoint.address().is_unspecified()) {
        acceptor_.set_option(asio::ip::v6_only(false));
    }
    acceptor_.bind(endpoint);
    acceptor_.listen();
    asio::co_spawn(strand_, accept(shared_from_this()), [](const std::exception_ptr &) {});
}

void HttpSignalingServer::close()
{
    asio::post(strand_, [self = shared_from_this()] {
        asio::error_code ignored;
        std::ignore = self->acceptor_.close(ignored);
        for (auto *socket : self->connections_) {
            std::ignore = socket->close(ignored);
        }
    });
}

asio::awaitable<void> HttpSignalingServer::accept(std::shared_ptr<HttpSignalingServer> self)
{
    while (self->acceptor_.is_open()) {
        auto [error, socket] = co_await self->acceptor_.async_accept(AWAITABLE);
        if (error) {
            asio::steady_timer pause(self->strand_, std::chrono::milliseconds{100});
            co_await pause.async_wait(AWAITABLE);
            continue;
        }
        asio::co_spawn(self->strand_, serve(self, std::move(socket)), [](const std::exception_ptr &) {});
    }
}

asio::awaitable<void> HttpSignalingServer::serve(std::shared_ptr<HttpSignalingServer> self,
                                                 asio::ip::tcp::socket socket)
{
    const Registration registration(self->connections_, socket);
    const auto deadline = std::chrono::steady_clock::now() + self->config_.request_timeout;
    asio::error_code ignored;
    std::ignore = socket.set_option(asio::ip::tcp::no_delay(true), ignored);

    std::array<std::uint8_t, 1> first{};
    const auto [peek_error, peeked] = co_await socket.async_receive(
        asio::buffer(first), asio::socket_base::message_peek, asio::cancel_after(remainingUntil(deadline), AWAITABLE));
    if (peek_error || peeked == 0) {
        co_return;
    }

    constexpr std::uint8_t handshake_record = 0x16;
    if (first[0] != handshake_record) {
        co_await self->exchange(socket, deadline);
        co_return;
    }
    if (!self->tls_) {
        std::array<char, 16384> client_hello{};
        co_await socket.async_read_some(asio::buffer(client_hello),
                                        asio::cancel_after(std::chrono::milliseconds{100}, AWAITABLE));
        constexpr std::array<std::uint8_t, 7> handshake_failure{0x15, 0x03, 0x01, 0x00, 0x02, 0x02, 0x28};
        co_await asio::async_write(socket, asio::buffer(handshake_failure),
                                   asio::cancel_after(self->config_.write_timeout, AWAITABLE));
        co_return;
    }

    asio::ssl::stream<asio::ip::tcp::socket &> stream(socket, *self->tls_);
    const auto [handshake_error] = co_await stream.async_handshake(
        asio::ssl::stream_base::server, asio::cancel_after(remainingUntil(deadline), AWAITABLE));
    if (handshake_error) {
        co_return;
    }
    co_await self->exchange(stream, deadline);
    co_await stream.async_shutdown(asio::cancel_after(std::chrono::seconds{1}, AWAITABLE));
}

template <typename Stream>
asio::awaitable<void> HttpSignalingServer::exchange(Stream &stream, std::chrono::steady_clock::time_point deadline)
{
    std::string buffer;
    std::optional<Request> request;
    while (!request) {
        std::array<char, 4096> chunk{};
        const auto [error, received] = co_await stream.async_read_some(
            asio::buffer(chunk), asio::cancel_after(remainingUntil(deadline), AWAITABLE));
        if (error || buffer.size() + received > config_.max_request_bytes) {
            co_return;
        }
        buffer.append(chunk.data(), received);

        const auto header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            continue;
        }
        std::string_view head(buffer.data(), header_end + 2);
        const auto request_line = head.substr(0, head.find("\r\n"));
        head.remove_prefix(request_line.size() + 2);
        const auto method_end = request_line.find(' ');
        const auto path_end =
            method_end == std::string_view::npos ? method_end : request_line.find(' ', method_end + 1);
        if (path_end == std::string_view::npos) {
            co_return;
        }
        const auto method = request_line.substr(0, method_end);
        const auto target = request_line.substr(method_end + 1, path_end - method_end - 1);
        const auto query_start = target.find('?');
        const auto path = target.substr(0, query_start);
        const auto query = query_start == std::string_view::npos ? std::string_view{} : target.substr(query_start + 1);
        if (method.empty() || method.size() > 16 ||
            !std::ranges::all_of(method, [](char c) { return c >= 'A' && c <= 'Z'; }) || !path.starts_with('/')) {
            co_return;
        }

        std::optional<std::size_t> content_length;
        std::optional<std::string_view> host;
        while (!head.empty()) {
            const auto header = head.substr(0, head.find("\r\n"));
            head.remove_prefix(std::min(head.size(), header.size() + 2));
            const auto colon = header.find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            const auto name = header.substr(0, colon);
            const auto lowercase_equal = [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == b;
            };
            const auto is_content_length =
                std::ranges::equal(name, std::string_view{"content-length"}, lowercase_equal);
            const auto is_host = std::ranges::equal(name, std::string_view{"host"}, lowercase_equal);
            if (!is_content_length && !is_host) {
                continue;
            }
            auto value = header.substr(colon + 1);
            value.remove_prefix(std::min(value.find_first_not_of(" \t"), value.size()));
            value.remove_suffix(value.size() - std::min(value.find_last_not_of(" \t") + 1, value.size()));
            if (is_host) {
                if (host) {
                    co_return;
                }
                host = value;
                continue;
            }
            std::size_t length = 0;
            const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), length);
            if (parse_error != std::errc{} || end != value.data() + value.size() ||
                (content_length && *content_length != length)) {
                co_return;
            }
            content_length = length;
        }

        std::string_view host_name;
        std::optional<std::uint16_t> host_port;
        if (host) {
            auto authority = *host;
            if (authority.starts_with('[')) {
                const auto bracket = authority.find(']');
                if (bracket == std::string_view::npos) {
                    co_return;
                }
                host_name = authority.substr(1, bracket - 1);
                authority.remove_prefix(bracket + 1);
            }
            else {
                const auto colon = authority.find(':');
                host_name = authority.substr(0, colon);
                authority.remove_prefix(colon == std::string_view::npos ? authority.size() : colon);
            }
            if (!authority.empty()) {
                std::uint16_t port = 0;
                const auto [end, parse_error] =
                    std::from_chars(authority.data() + 1, authority.data() + authority.size(), port);
                if (authority.front() != ':' || parse_error != std::errc{} ||
                    end != authority.data() + authority.size()) {
                    co_return;
                }
                host_port = port;
            }
            if (host_name.empty()) {
                co_return;
            }
        }

        const auto body_length = content_length.value_or(0);
        if (body_length > config_.max_request_bytes - header_end - 4) {
            co_return;
        }
        if (buffer.size() < header_end + 4 + body_length) {
            continue;
        }
        request = Request{.method = std::string(method),
                          .path = std::string(path),
                          .query = std::string(query),
                          .host = std::string(host_name),
                          .port = host_port,
                          .body = buffer.substr(header_end + 4, body_length)};
    }

    const auto response = co_await respond(std::move(*request));
    const auto serialized =
        std::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: "
                    "close\r\n\r\n{}",
                    response.status, response.reason, response.content_type, response.body.size(), response.body);
    co_await asio::async_write(stream, asio::buffer(serialized), asio::cancel_after(config_.write_timeout, AWAITABLE));
}

asio::awaitable<HttpSignalingServer::Response> HttpSignalingServer::respond(Request request)
{
    const auto error = [](std::uint16_t status, std::string_view message) {
        return Response{.status = status,
                        .reason = std::string(message),
                        .content_type = "text/plain",
                        .body = std::string(message)};
    };

    if (request.method == "GET" && request.path == "/v1/join") {
        PingRequest ping_request;
        for (const auto parameter : std::views::split(std::string_view{request.query}, '&')) {
            const std::string_view pair{parameter.begin(), parameter.end()};
            const auto equals = pair.find('=');
            const auto name = pair.substr(0, equals);
            const auto value = equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);
            if (name == "protocol") {
                int protocol = 0;
                const auto [end, parse_error] = std::from_chars(value.data(), value.data() + value.size(), protocol);
                if (parse_error == std::errc{} && end == value.data() + value.size()) {
                    ping_request.protocol = protocol;
                }
            }
            else if (name == "version") {
                ping_request.version = value;
            }
            else if (name == "id") {
                ping_request.network_id = value;
            }
        }
        const auto ping = handler_.getServerPing(ping_request);
        std::string body;
        if (glz::write_json(Ping{.name = ping.getDescription(),
                                 .protocol = ping.getVersion().getProtocol(),
                                 .version = ping.getVersion().getName(),
                                 .level = ping.getLevelName(),
                                 .players = ping.getPlayers().getOnline(),
                                 .max_players = ping.getPlayers().getMax(),
                                 .game_type = static_cast<int>(ping.getGameType())},
                            body)) {
            co_return error(500, "Internal Server Error");
        }
        co_return Response{.status = 200, .reason = "OK", .content_type = "application/json", .body = std::move(body)};
    }

    constexpr std::string_view join_path = "/v1/join/";
    if (request.method != "POST" || !request.path.starts_with(join_path)) {
        co_return error(404, "Not Found");
    }
    const auto network_id = std::string_view(request.path).substr(join_path.size());
    if (network_id.empty() || network_id.size() > 128 || !std::ranges::all_of(network_id, [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_' || c == '~' ||
                   c == '%';
        })) {
        co_return error(400, "Expected /v1/join/{networkId}");
    }
    if (request.body.empty()) {
        co_return error(400, "Missing SDP offer in request body");
    }

    auto pending = std::make_shared<JoinResponse::Pending>(strand_);
    pending->timer.expires_after(config_.join_timeout);
    handler_.onJoin(JoinRequest{.network_id = std::string(network_id),
                                .offer = std::move(request.body),
                                .host = std::move(request.host),
                                .port = request.port},
                    JoinResponse{pending});
    co_await pending->timer.async_wait(AWAITABLE);
    if (!pending->result) {
        co_return error(504, "Gateway Timeout");
    }
    if (const auto *const code = std::get_if<std::uint32_t>(&*pending->result)) {
        co_return Response{
            .status = 200, .reason = "OK", .content_type = "application/sdp", .body = std::to_string(*code)};
    }
    co_return Response{.status = 200,
                       .reason = "OK",
                       .content_type = "application/sdp",
                       .body = std::move(std::get<std::string>(*pending->result))};
}

}  // namespace endgate::nethernet
