#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <spdlog/logger.h>

#include "endgate/connection/connection.h"
#include "endgate/network/raknet/connection.h"

namespace endgate {

/**
 * A leg carried over RakNet: a player that connected to an endpoint's socket, or a connection the
 * proxy opens to a backend on a socket of its own. RakNet puts 0xFE in front of every batch, so the
 * leg takes it off what it receives, drops a message without it, and puts it back on what it sends.
 *
 * @see RakNet::RakPeer::RemoteSystemStruct
 */
class RakNetConnection final : public MinecraftConnection, private raknet::Connection::Listener {
public:
    RakNetConnection(Endpoint &endpoint, const raknet::SystemAddress &address,
                     const raknet::OpenConnectionRequest2 &request, std::uint64_t guid, std::uint16_t system_index,
                     raknet::TimePoint now);

    RakNetConnection(Endpoint &endpoint, const raknet::SystemAddress &address, std::uint64_t guid,
                     raknet::TimePoint now);

    /**
     * Opens a backend connection's socket and sends its first request.
     *
     * @see RakNet::RakPeer::Connect
     */
    void connect();

protected:
    void handleDatagram(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender) override;
    void send(ByteBuf batch) override;
    void disconnect(bool wait_for_peer) override;

private:
    void sendDatagram(std::span<const std::byte> datagram) override;
    bool sendMtuProbe(std::span<const std::byte> datagram) override;
    void onConnected() override;
    void onMessage(ByteBuf message) override;
    void onDisconnected(raknet::DisconnectReason reason) override;
    void onReliabilityLayerNotification(std::string_view message, bool is_error) override;

    static asio::awaitable<void> read(std::shared_ptr<RakNetConnection> self);
    void arm();

    spdlog::logger &logger_;
    std::shared_ptr<asio::ip::udp::socket> endpoint_socket_;
    asio::ip::udp::socket socket_;
    raknet::SystemAddress address_;
    asio::steady_timer timer_;
    std::optional<raknet::TimePoint> armed_;
    bool active_{false};
    raknet::Connection connection_;
};

}  // namespace endgate
