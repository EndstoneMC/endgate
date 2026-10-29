#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include <asio/io_context.hpp>
#include <asio/ssl/context.hpp>

#include "endgate/awaitable.h"
#include "endgate/network/nethernet/session.h"
#include "endgate/network/ping.h"

namespace endgate::nethernet {

/**
 * A NetherNet HTTP signalling client: the dialer's side of the signalling exchange a Bedrock client
 * runs. It probes a target for the signalling capability and trades an SDP offer for an answer, trying
 * TLS with SNI first and falling back to plain HTTP when the handshake is refused, exactly as the
 * client does.
 *
 * @see NetherNet::HttpSignalingClient
 */
struct SignalingClient {
    struct Config {
        std::chrono::milliseconds connect_timeout{std::chrono::seconds{5}};
        std::chrono::milliseconds request_timeout{std::chrono::seconds{10}};
        std::chrono::milliseconds tls_handshake_timeout{std::chrono::seconds{5}};
        std::size_t max_response_bytes{65536};
        std::shared_ptr<asio::ssl::context> tls;
    };

    /**
     * The server identity the answer carried, structurally verified: the JWT self-signature and the
     * fingerprints JWS both check out against the cpk the JWT embeds.
     *
     * @see NetherNet::ServerIdentityAssertionVerifier
     */
    struct ServerAssertion {
        std::string idp_domain;
        std::string cpk_digest;
        bool verified{false};
    };

    struct Probe {
        bool supported{false};
        std::optional<ServerPing> ping;
    };

    struct Answer {
        std::string sdp;
        std::optional<ServerAssertion> identity;
    };

    /**
     * Does the target support NetherNet HTTP signalling? A 2xx to `GET /v1/join` means yes. This is the
     * reusable backend-transport detector; it costs one request and returns the ping too.
     *
     * @see NetherNet::NetworkSessionManager::_discover
     */
    [[nodiscard]] static Awaitable<Probe> probeNetherNet(asio::io_context &io_context, std::string host,
                                                         std::uint16_t port, Config config);

    /**
     * Dial a NetherNet backend: `GET /v1/join` to confirm the capability, then `POST
     * /v1/join/{networkId}` with the offer SDP, returning the answer or the NetherNet error the body
     * carried.
     *
     * @see NetherNet::HttpSignalingServer::SendSignal
     */
    [[nodiscard]] static Awaitable<std::expected<Answer, ESessionError>> requestAnswer(
        asio::io_context &io_context, std::string host, std::uint16_t port, std::string network_id, std::string offer,
        Config config);
};

}  // namespace endgate::nethernet
