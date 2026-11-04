#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/ssl/context.hpp>

#include "endgate/awaitable.h"
#include "endgate/network/eventloop.h"

namespace endgate {

/**
 * @see java.net.http.HttpRequest
 */
struct HttpRequest {
    std::string uri;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string method = "GET";
    std::string body;
};

/**
 * @see java.net.http.HttpResponse
 */
class HttpResponse {
public:
    HttpResponse() = default;
    HttpResponse(int status_code, std::vector<std::pair<std::string, std::string>> headers, std::string body)
        : status_code_(status_code), headers_(std::move(headers)), body_(std::move(body))
    {
    }

    /**
     * @see java.net.http.HttpResponse#statusCode
     */
    [[nodiscard]] int statusCode() const { return status_code_; }

    /**
     * @see java.net.http.HttpHeaders#firstValue
     */
    [[nodiscard]] std::optional<std::string_view> firstValue(std::string_view name) const;

    /**
     * @see java.net.http.HttpResponse#body
     */
    [[nodiscard]] const std::string &body() const { return body_; }

private:
    int status_code_{0};
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
};

/**
 * Sends HTTP/1.1 requests on its event loop, over TLS for an https URI, checking the server's
 * certificate against the system's trust store and its name against the host.
 *
 * @see java.net.http.HttpClient
 */
class HttpClient {
public:
    struct Config {
        std::chrono::milliseconds timeout{std::chrono::seconds{10}};
        std::size_t max_response_bytes{1024 * 1024};
    };

    HttpClient(EventLoop &executor, Config config);

    /**
     * @see java.net.http.HttpClient#executor
     */
    [[nodiscard]] EventLoop &executor() const { return executor_; }

    /**
     * @see java.net.http.HttpClient#sendAsync
     */
    [[nodiscard]] Awaitable<std::expected<HttpResponse, std::string>> sendAsync(HttpRequest request) const;

private:
    EventLoop &executor_;
    Config config_;
    std::shared_ptr<asio::ssl::context> tls_;
};

}  // namespace endgate
