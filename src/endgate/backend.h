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
 * connection to it fails.
 *
 * @see com.velocitypowered.api.proxy.server.RegisteredServer
 * @see com.velocitypowered.proxy.server.VelocityRegisteredServer
 */
class BackendServer {
public:
    explicit BackendServer(ServerInfo info);

    [[nodiscard]] const ServerInfo &getServerInfo() const { return info_; }

    [[nodiscard]] Awaitable<std::optional<ServerInfo::Transport>> detectTransport(asio::io_context &io_context,
                                                                                  asio::ip::udp::endpoint address);

    void forgetTransport();

    /**
     * Sends RakNet unconnected pings until one is answered.
     *
     * @see com.velocitypowered.proxy.server.VelocityRegisteredServer#ping
     * @see RakNet::RakPeer::Ping
     */
    [[nodiscard]] static Awaitable<bool> ping(asio::ip::udp::endpoint address, std::chrono::milliseconds timeout);

private:
    ServerInfo info_;
    spdlog::logger &logger_;
    std::mutex mutex_;
    std::optional<ServerInfo::Transport> detected_;
};

}  // namespace endgate
