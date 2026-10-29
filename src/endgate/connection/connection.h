#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <asio/ip/udp.hpp>
#include <spdlog/logger.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/cipher.h"
#include "endgate/network/compress.h"
#include "endgate/network/eventloop.h"
#include "endgate/network/frame.h"
#include "endgate/network/packet.h"
#include "endgate/network/pipeline.h"

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
    virtual void handleUnknown(ByteBufView batch) = 0;

    /**
     * Called once a read has delivered everything it carried.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#readCompleted
     */
    virtual void readCompleted() {}

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#disconnected
     */
    virtual void disconnected() = 0;
};

/**
 * One leg of a proxied session, a player's or a backend's, whatever transport carries it. A leg is
 * pinned to one event loop for life: write, flush, close and shutdown may be called from any thread
 * and run inline on that loop, or are handed to it. A batch crosses this interface as NetherNet
 * carries it, with no RakNet 0xFE in front; a transport that frames batches otherwise converts at its
 * own edge. Until login switches the stages on, cipher, compression, framing and decoding pass a
 * batch through unopened, which is how the proxy forwards what it cannot read.
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
     * Writes and flushes.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#write
     */
    void write(ByteBuf batch);

    /**
     * Writes without flushing.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#delayedWrite
     */
    void delayedWrite(ByteBuf batch);

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#flush
     */
    void flush();

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
    void channelRead(ByteBufView batch);

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#channelReadComplete
     */
    void channelReadComplete();

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#channelInactive
     */
    void channelInactive();

private:
    /**
     * Where the pipeline ends: events reaching its tail are handled by the connection, and operations
     * reaching its head are sent by the transport.
     *
     * @see io.netty.channel.ChannelPipeline
     */
    class Sink {
    public:
        explicit Sink(MinecraftConnection &connection) : connection_(connection) {}

        void channelActive();
        void channelInactive();
        void channelRead(ByteBufView batch);
        void channelReadComplete();
        void exceptionCaught(const std::exception &cause);
        void write(ByteBuf batch);
        void flush() {}
        void close();

    private:
        MinecraftConnection &connection_;
    };

    using Pipeline = ChannelPipeline<Sink, MinecraftCipherDecoder, MinecraftCipherEncoder, MinecraftCompressDecoder,
                                     MinecraftCompressEncoder, MinecraftVarintFrameDecoder,
                                     MinecraftVarintLengthEncoder, MinecraftDecoder<>, MinecraftEncoder<>>;

    Endpoint &endpoint_;
    EventLoop &event_loop_;
    spdlog::logger &logger_;
    std::string remote_address_;
    Sink sink_;
    Pipeline pipeline_;
    std::shared_ptr<MinecraftSessionHandler> handler_;
};

}  // namespace endgate
