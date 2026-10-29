#include "endgate/network/http.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <format>
#include <system_error>
#include <tuple>

#include <asio/as_tuple.hpp>
#include <asio/cancel_after.hpp>
#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

namespace endgate {

namespace {

constexpr auto AWAITABLE = asio::as_tuple(asio::use_awaitable);

bool equalsIgnoringCase(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

struct Uri {
    bool secure{false};
    std::string host;
    std::string port;
    std::string target;

    static std::optional<Uri> parse(std::string_view uri)
    {
        Uri parsed;
        if (uri.starts_with("https://")) {
            parsed.secure = true;
            uri.remove_prefix(8);
        }
        else if (uri.starts_with("http://")) {
            uri.remove_prefix(7);
        }
        else {
            return std::nullopt;
        }
        const auto path = uri.find_first_of("/?");
        auto authority = uri.substr(0, path);
        parsed.target = path == std::string_view::npos ? "/" : std::string(uri.substr(path));
        if (parsed.target.starts_with('?')) {
            parsed.target.insert(0, "/");
        }
        std::string_view port;
        if (authority.starts_with('[')) {
            const auto close = authority.find(']');
            if (close == std::string_view::npos) {
                return std::nullopt;
            }
            parsed.host = authority.substr(1, close - 1);
            const auto rest = authority.substr(close + 1);
            if (!rest.empty() && !rest.starts_with(':')) {
                return std::nullopt;
            }
            port = rest.empty() ? rest : rest.substr(1);
        }
        else {
            const auto colon = authority.rfind(':');
            parsed.host = authority.substr(0, colon);
            port = colon == std::string_view::npos ? std::string_view{} : authority.substr(colon + 1);
        }
        const std::string_view default_port = parsed.secure ? "443" : "80";
        parsed.port = port.empty() ? default_port : port;
        if (parsed.host.empty() || !std::ranges::all_of(parsed.port, [](char c) { return std::isdigit(c) != 0; })) {
            return std::nullopt;
        }
        return parsed;
    }
};

std::expected<std::string, std::string> decodeChunked(std::string_view body)
{
    std::string decoded;
    while (true) {
        const auto line_end = body.find("\r\n");
        if (line_end == std::string_view::npos) {
            return std::unexpected("truncated chunk size");
        }
        const auto size_text = body.substr(0, std::min(line_end, body.find(';')));
        std::size_t size = 0;
        const auto [end, error] = std::from_chars(size_text.data(), size_text.data() + size_text.size(), size, 16);
        if (error != std::errc{} || end != size_text.data() + size_text.size()) {
            return std::unexpected("malformed chunk size");
        }
        body.remove_prefix(line_end + 2);
        if (size == 0) {
            return decoded;
        }
        if (body.size() < size + 2 || body.substr(size, 2) != "\r\n") {
            return std::unexpected("truncated chunk");
        }
        decoded.append(body.substr(0, size));
        body.remove_prefix(size + 2);
    }
}

std::expected<HttpResponse, std::string> parseResponse(std::string_view raw)
{
    const auto head_end = raw.find("\r\n\r\n");
    if (head_end == std::string_view::npos) {
        return std::unexpected("truncated response headers");
    }
    auto head = raw.substr(0, head_end + 2);
    const auto status_line = head.substr(0, head.find("\r\n"));
    head.remove_prefix(status_line.size() + 2);
    const auto space = status_line.find(' ');
    const auto code = space == std::string_view::npos ? std::string_view{} : status_line.substr(space + 1, 3);
    int status_code = 0;
    if (!status_line.starts_with("HTTP/1.") || code.size() != 3 ||
        std::from_chars(code.data(), code.data() + code.size(), status_code).ec != std::errc{}) {
        return std::unexpected("malformed status line");
    }

    std::vector<std::pair<std::string, std::string>> headers;
    while (!head.empty()) {
        const auto line = head.substr(0, head.find("\r\n"));
        head.remove_prefix(line.size() + 2);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        auto value = line.substr(colon + 1);
        value.remove_prefix(std::min(value.find_first_not_of(" \t"), value.size()));
        value.remove_suffix(value.size() - std::min(value.find_last_not_of(" \t") + 1, value.size()));
        headers.emplace_back(line.substr(0, colon), value);
    }
    const auto header = [&headers](std::string_view name) -> std::optional<std::string_view> {
        const auto found =
            std::ranges::find_if(headers, [name](const auto &entry) { return equalsIgnoringCase(entry.first, name); });
        return found == headers.end() ? std::nullopt : std::optional<std::string_view>{found->second};
    };

    auto body = raw.substr(head_end + 4);
    std::string content;
    if (const auto encoding = header("transfer-encoding"); encoding && equalsIgnoringCase(*encoding, "chunked")) {
        auto decoded = decodeChunked(body);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        content = std::move(*decoded);
    }
    else if (const auto length_text = header("content-length")) {
        std::size_t length = 0;
        if (std::from_chars(length_text->data(), length_text->data() + length_text->size(), length).ec != std::errc{} ||
            length > body.size()) {
            return std::unexpected("truncated response body");
        }
        content = body.substr(0, length);
    }
    else {
        content = body;
    }
    return HttpResponse(status_code, std::move(headers), std::move(content));
}

template <typename Stream>
Awaitable<std::expected<std::string, std::string>> exchange(Stream &stream, const std::string &request,
                                                            const HttpClient::Config &config)
{
    const auto [write_error, written] =
        co_await asio::async_write(stream, asio::buffer(request), asio::cancel_after(config.timeout, AWAITABLE));
    if (write_error) {
        co_return std::unexpected(write_error.message());
    }
    std::string raw;
    while (true) {
        std::array<char, 16384> chunk{};
        const auto [error, received] =
            co_await stream.async_read_some(asio::buffer(chunk), asio::cancel_after(config.timeout, AWAITABLE));
        raw.append(chunk.data(), received);
        if (raw.size() > config.max_response_bytes) {
            co_return std::unexpected("response too large");
        }
        if (error == asio::error::eof || error == asio::ssl::error::stream_truncated) {
            co_return raw;
        }
        if (error) {
            co_return std::unexpected(error.message());
        }
    }
}

Awaitable<std::expected<HttpResponse, std::string>> send(HttpRequest request, HttpClient::Config config,
                                                         std::shared_ptr<asio::ssl::context> tls)
{
    const auto uri = Uri::parse(request.uri);
    if (!uri) {
        co_return std::unexpected("invalid URI " + request.uri);
    }
    auto message = std::format("GET {} HTTP/1.1\r\nHost: {}\r\n", uri->target, uri->host);
    for (const auto &[name, value] : request.headers) {
        message += std::format("{}: {}\r\n", name, value);
    }
    message += "Connection: close\r\n\r\n";

    const auto executor = co_await asio::this_coro::executor;
    asio::ip::tcp::resolver resolver(executor);
    const auto [resolve_error, endpoints] =
        co_await resolver.async_resolve(uri->host, uri->port, asio::cancel_after(config.timeout, AWAITABLE));
    if (resolve_error) {
        co_return std::unexpected(resolve_error.message());
    }
    asio::ip::tcp::socket socket(executor);
    const auto [connect_error, connected] =
        co_await asio::async_connect(socket, endpoints, asio::cancel_after(config.timeout, AWAITABLE));
    if (connect_error) {
        co_return std::unexpected(connect_error.message());
    }

    std::expected<std::string, std::string> raw;
    if (!uri->secure) {
        raw = co_await exchange(socket, message, config);
    }
    else {
        asio::ssl::stream<asio::ip::tcp::socket &> stream(socket, *tls);
        asio::error_code literal;
        std::ignore = asio::ip::make_address(uri->host, literal);
        if (literal) {
            SSL_set_tlsext_host_name(stream.native_handle(), uri->host.c_str());
        }
        stream.set_verify_callback(asio::ssl::host_name_verification(uri->host));
        const auto [handshake_error] = co_await stream.async_handshake(asio::ssl::stream_base::client,
                                                                       asio::cancel_after(config.timeout, AWAITABLE));
        if (handshake_error) {
            co_return std::unexpected(handshake_error.message());
        }
        raw = co_await exchange(stream, message, config);
    }
    if (!raw) {
        co_return std::unexpected(raw.error());
    }
    co_return parseResponse(*raw);
}

}  // namespace

std::optional<std::string_view> HttpResponse::firstValue(std::string_view name) const
{
    const auto header =
        std::ranges::find_if(headers_, [name](const auto &entry) { return equalsIgnoringCase(entry.first, name); });
    if (header == headers_.end()) {
        return std::nullopt;
    }
    return header->second;
}

HttpClient::HttpClient(EventLoop &executor, Config config)
    : executor_(executor), config_(config), tls_(std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client))
{
    constexpr std::array<std::string_view, 3> bundles{
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem",
    };
    tls_->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                      asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
    tls_->set_default_verify_paths();
    for (const auto bundle : bundles) {
        std::error_code exists_error;
        asio::error_code ignored;
        if (std::filesystem::exists(bundle, exists_error)) {
            std::ignore = tls_->load_verify_file(std::string(bundle), ignored);
        }
    }
    tls_->set_verify_mode(asio::ssl::verify_peer);
}

Awaitable<std::expected<HttpResponse, std::string>> HttpClient::sendAsync(HttpRequest request) const
{
    co_return co_await asio::co_spawn(executor_.getExecutor(), send(std::move(request), config_, tls_),
                                      asio::use_awaitable);
}

}  // namespace endgate
