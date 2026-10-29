#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/steady_timer.hpp>

#include "endgate/awaitable.h"
#include "endgate/network/eventloop.h"
#include "endgate/network/ping.h"

namespace endgate::nethernet {

/**
 * Completes one join with an SDP answer or a NetherNet error code, once, from any thread. A join
 * that is never completed times out.
 *
 * @see NetherNet::HttpSignalingServer::SendSignal
 */
class JoinResponse {
public:
    struct Pending {
        explicit Pending(EventLoop &loop) : event_loop(loop), timer(loop.getExecutor()) {}

        EventLoop &event_loop;
        asio::steady_timer timer;
        std::optional<std::variant<std::string, std::uint32_t>> result;
    };

    explicit JoinResponse(std::shared_ptr<Pending> pending) : pending_(std::move(pending)) {}

    void answer(std::string sdp) const;

    /**
     * @see NetherNet::ESessionError
     */
    void reject(std::uint32_t session_error) const;

private:
    std::shared_ptr<Pending> pending_;
};

/**
 * One GET /v1/join, with what the client says about itself in the query string.
 */
struct PingRequest {
    std::optional<int> protocol;
    std::string version;
    std::string network_id;
};

/**
 * One POST /v1/join/{networkId}, with the address the client reached the proxy at, taken from its Host
 * header.
 */
struct JoinRequest {
    std::string network_id;
    std::string offer;
    std::string host;
    std::optional<std::uint16_t> port;
};

/**
 * What the signalling server needs from the proxy: the ping it shows, and an answer to every offer.
 */
class JoinHandler {
public:
    virtual ~JoinHandler() = default;
    [[nodiscard]] virtual ServerPing getServerPing(const PingRequest &request) = 0;
    virtual void onJoin(JoinRequest request, JoinResponse response) = 0;
};

/**
 * NetherNet's HTTP signalling, as BDS serves it on its server port: GET /v1/join for the ping and
 * POST /v1/join/{networkId} to trade an SDP offer for an answer. A connection whose first byte opens a
 * TLS record is served over TLS when a certificate is configured, and refused at once when not.
 *
 * @see NetherNet::HttpSignalingServer
 * @see NetherNet::HttpServer
 */
class HttpSignalingServer : public std::enable_shared_from_this<HttpSignalingServer> {
public:
    struct Config {
        std::size_t max_request_bytes{65536};
        std::chrono::milliseconds request_timeout{std::chrono::seconds{10}};
        std::chrono::milliseconds write_timeout{std::chrono::seconds{10}};
        std::chrono::milliseconds join_timeout{std::chrono::seconds{10}};
    };

    [[nodiscard]] static std::shared_ptr<HttpSignalingServer> create(EventLoop &event_loop, JoinHandler &handler,
                                                                     Config config,
                                                                     std::shared_ptr<asio::ssl::context> tls);

    [[nodiscard]] static std::shared_ptr<asio::ssl::context> loadTls(const std::filesystem::path &certificate_chain,
                                                                     const std::filesystem::path &private_key);

    void listen(const asio::ip::tcp::endpoint &endpoint);
    void close();

    [[nodiscard]] asio::ip::tcp::endpoint getLocalEndpoint() const { return acceptor_.local_endpoint(); }

private:
    struct Request {
        std::string method;
        std::string path;
        std::string query;
        std::string host;
        std::optional<std::uint16_t> port;
        std::string body;
    };

    struct Response {
        std::uint16_t status;
        std::string reason;
        std::string content_type;
        std::string body;
    };

    HttpSignalingServer(EventLoop &event_loop, JoinHandler &handler, Config config,
                        std::shared_ptr<asio::ssl::context> tls);

    static Awaitable<void> accept(std::shared_ptr<HttpSignalingServer> self);
    static Awaitable<void> serve(std::shared_ptr<HttpSignalingServer> self, asio::ip::tcp::socket socket);

    template <typename Stream>
    Awaitable<void> exchange(Stream &stream, std::chrono::steady_clock::time_point deadline);

    Awaitable<Response> respond(Request request);

    EventLoop &event_loop_;
    asio::ip::tcp::acceptor acceptor_;
    JoinHandler &handler_;
    Config config_;
    std::shared_ptr<asio::ssl::context> tls_;
    std::unordered_set<asio::ip::tcp::socket *> connections_;
};

}  // namespace endgate::nethernet
