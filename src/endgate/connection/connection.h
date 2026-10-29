#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <asio/ip/udp.hpp>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/eventloop.h"

namespace endgate {

class Endpoint;

[[nodiscard]] inline std::string toString(const asio::ip::udp::endpoint &address)
{
    return address.address().to_string() + ":" + std::to_string(address.port());
}

/**
 * Hears what a connection does, always on the connection's event loop.
 *
 * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler
 */
class MinecraftSessionHandler {
public:
    virtual ~MinecraftSessionHandler() = default;

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#connected
     */
    virtual void connected() = 0;

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#handleUnknown
     */
    virtual void handleUnknown(ByteBuf batch) = 0;

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#disconnected
     */
    virtual void disconnected() = 0;
};

/**
 * One leg of a proxied session, a player's or a backend's, whatever transport carries it. A leg is
 * pinned to one event loop for life: write, close and shutdown may be called from any thread and run
 * inline on that loop, or are handed to it. A batch crosses this interface as NetherNet carries it,
 * with no RakNet 0xFE in front; a transport that frames batches otherwise converts at its own edge.
 *
 * @see com.velocitypowered.proxy.connection.MinecraftConnection
 * @see io.netty.channel.Channel
 */
class MinecraftConnection : public std::enable_shared_from_this<MinecraftConnection> {
public:
    using Clock = std::chrono::steady_clock;

    MinecraftConnection(Endpoint &endpoint, std::string remote_address);
    MinecraftConnection(const MinecraftConnection &) = delete;
    MinecraftConnection &operator=(const MinecraftConnection &) = delete;
    MinecraftConnection(MinecraftConnection &&) = delete;
    MinecraftConnection &operator=(MinecraftConnection &&) = delete;
    virtual ~MinecraftConnection() = default;

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#write
     */
    void write(ByteBuf batch);

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#close
     */
    void close();

    /**
     * Closes without waiting for the peer to acknowledge it.
     *
     * @see RakNet::RakPeer::Shutdown
     */
    void shutdown();

    /**
     * Takes a datagram its sender sent to the connection, on the connection's loop.
     *
     * @see io.netty.channel.ChannelInboundHandler#channelRead
     */
    virtual void handleDatagram(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender) = 0;

    /**
     * Call before the connection starts or on its event loop.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#setActiveSessionHandler
     */
    void setSessionHandler(std::shared_ptr<MinecraftSessionHandler> handler);

    /**
     * @see io.netty.channel.Channel#eventLoop
     */
    [[nodiscard]] EventLoop &eventLoop() const { return event_loop_; }
    [[nodiscard]] Endpoint &getEndpoint() const { return endpoint_; }

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#getRemoteAddress
     */
    [[nodiscard]] const std::string &getRemoteAddress() const { return remote_address_; }

protected:
    virtual void send(ByteBuf batch) = 0;
    virtual void disconnect(bool wait_for_peer) = 0;

    void setRemoteAddress(std::string remote_address) { remote_address_ = std::move(remote_address); }

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#channelActive
     */
    void channelActive();

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#channelRead
     */
    void channelRead(ByteBuf batch);

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#channelInactive
     */
    void channelInactive();

private:
    Endpoint &endpoint_;
    EventLoop &event_loop_;
    std::string remote_address_;
    std::shared_ptr<MinecraftSessionHandler> handler_;
};

}  // namespace endgate
