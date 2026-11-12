#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <spdlog/logger.h>

#include "endgate/awaitable.h"
#include "endgate/network/ping.h"

namespace endgate {

/**
 * A backend as configured: a name, where it is, and the transport it is pinned to with a
 * nethernet:// or raknet:// prefix, if any.
 *
 * @see com.velocitypowered.api.proxy.server.ServerInfo
 */
class ServerInfo {
public:
    enum class Transport {
        RakNet,
        NetherNet,
    };

    [[nodiscard]] static std::optional<ServerInfo> parse(std::string name, std::string_view address);

    [[nodiscard]] const std::string &getName() const { return name_; }
    [[nodiscard]] const std::string &getAddress() const { return address_; }
    [[nodiscard]] const std::string &getHost() const { return host_; }
    [[nodiscard]] std::uint16_t getPort() const { return port_; }
    [[nodiscard]] std::optional<Transport> getTransport() const { return transport_; }

    bool operator==(const ServerInfo &other) const = default;

private:
    ServerInfo(std::string name, std::string address, std::string host, std::uint16_t port,
               std::optional<Transport> transport);

    std::string name_;
    std::string address_;
    std::string host_;
    std::uint16_t port_;
    std::optional<Transport> transport_;
};

[[nodiscard]] constexpr std::string_view toString(ServerInfo::Transport transport)
{
    switch (transport) {
    case ServerInfo::Transport::RakNet:
        return "RakNet";
    case ServerInfo::Transport::NetherNet:
        return "NetherNet";
    }
    return "unknown";
}

/**
 * A backend the proxy can send players to. Unless the configuration pins its transport, the first
 * connection to it finds out which one it supports, and every connection after uses that answer until a
 * connection to it fails. Either way, the first ping it answers tells the protocol it speaks.
 *
 * @see com.velocitypowered.api.proxy.server.RegisteredServer
 * @see com.velocitypowered.proxy.server.VelocityRegisteredServer
 */
struct PingTimeouts {
    std::chrono::milliseconds connect{2000};
    std::chrono::milliseconds read{1000};
};

struct TransportDetection {
    std::optional<ServerInfo::Transport> transport;
    std::optional<ServerPing> ping;
    bool attempted{false};
};

class BackendServer {
public:
    explicit BackendServer(ServerInfo info);

    [[nodiscard]] const ServerInfo &getServerInfo() const { return info_; }

    [[nodiscard]] Awaitable<TransportDetection> detectTransport(asio::io_context &io_context,
                                                                asio::ip::udp::endpoint address,
                                                                PingTimeouts timeouts = {});

    void forgetTransport();

    /**
     * The protocol the backend answered its last ping with, once one has been detected. A player is only
     * sent to a backend that speaks its own protocol.
     */
    [[nodiscard]] std::optional<int> getProtocol();

    /**
     * Whether the backend is known to speak a protocol other than the given one. A backend whose
     * protocol is still unknown is not.
     */
    [[nodiscard]] bool isOnOtherProtocol(int protocol);

    /**
     * Whether the backend answered a ping, and what its announcement said when it could be read. An
     * answer is what names the transport; the announcement is only where the protocol comes from.
     *
     * @see NetherNet::SignalingClient::Probe
     */
    struct Pong {
        bool answered{false};
        std::optional<ServerPing> ping;
    };

    /**
     * Asks the backend for its announcement, over the transport it uses.
     *
     * @see com.velocitypowered.proxy.server.VelocityRegisteredServer#ping
     */
    [[nodiscard]] Awaitable<std::optional<ServerPing>> ping(asio::io_context &io_context, PingTimeouts timeouts = {});

    /**
     * Sends RakNet unconnected pings until one is answered, and reads the announcement it carries.
     *
     * @see com.velocitypowered.proxy.server.VelocityRegisteredServer#ping
     * @see RakNet::RakPeer::Ping
     */
    [[nodiscard]] static Awaitable<Pong> ping(asio::ip::udp::endpoint address, std::chrono::milliseconds timeout);

private:
    void recordProtocol(const std::optional<ServerPing> &announcement);
    void warnWithoutProtocol();

    ServerInfo info_;
    spdlog::logger &logger_;
    std::mutex mutex_;
    std::optional<ServerInfo::Transport> detected_;
    std::optional<int> detected_protocol_;
    bool warned_ = false;
};

}  // namespace endgate
