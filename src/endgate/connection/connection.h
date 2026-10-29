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
#include "endgate/network/eventloop.h"
#include "endgate/network/pipeline.h"
#include "endgate/protocol/cipher.h"
#include "endgate/protocol/compress.h"
#include "endgate/protocol/frame.h"
#include "endgate/protocol/packet.h"
#include "endgate/protocol/state.h"

namespace endgate {

class Endpoint;

[[nodiscard]] inline std::string toString(const asio::ip::udp::endpoint &address)
{
    return address.address().to_string() + ":" + std::to_string(address.port());
}

template <typename List>
class PacketHandlers;

template <>
class PacketHandlers<TypeList<>> {
public:
    PacketHandlers() = default;
    PacketHandlers(const PacketHandlers &) = default;
    PacketHandlers &operator=(const PacketHandlers &) = default;
    PacketHandlers(PacketHandlers &&) = default;
    PacketHandlers &operator=(PacketHandlers &&) = default;
    virtual ~PacketHandlers() = default;
};

template <typename Packet, typename... Rest>
class PacketHandlers<TypeList<Packet, Rest...>> : public PacketHandlers<TypeList<Rest...>> {
public:
    /**
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#handle
     */
    virtual bool handle(const Packet &) { return false; }
};

template <typename Packet, typename Next, typename... Rest>
class PacketHandlers<TypeList<Packet, Next, Rest...>> : public PacketHandlers<TypeList<Next, Rest...>> {
public:
    using PacketHandlers<TypeList<Next, Rest...>>::handle;

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#handle
     */
    virtual bool handle(const Packet &) { return false; }
};

/**
 * Hears what a connection does, always on the connection's event loop.
 *
 * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler
 */
class MinecraftSessionHandler : public PacketHandlers<RegisteredPackets> {
public:
    /**
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#connected
     */
    virtual void connected() = 0;

    /**
     * Takes a registered packet no handle overload took, as the bytes it arrived in.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#handleGeneric
     */
    virtual void handleGeneric(ByteBufView packet) { handleUnknown(packet); }

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

    MinecraftConnection(Endpoint &endpoint, std::string remote_address, Direction direction);
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
     * Turns framing and decoding on, decoding what the state registers. Call on the connection's event
     * loop.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#setState
     */
    void setState(StateRegistry state);

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#getState
     */
    [[nodiscard]] StateRegistry getState() const { return state_; }

    /**
     * Call on the connection's event loop.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#setProtocolVersion
     */
    void setProtocolVersion(ProtocolVersion version);

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#getProtocolVersion
     */
    [[nodiscard]] ProtocolVersion getProtocolVersion() const { return protocol_version_; }

    /**
     * Compresses every batch from here on with the algorithm NetworkSettings names, those under the
     * threshold stored uncompressed, and decompresses each batch that arrives. Call on the connection's
     * event loop.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#setCompressionThreshold
     * @see NetworkSettingsPacket
     */
    void setCompressionThreshold(std::uint16_t threshold, PacketCompressionAlgorithm algorithm);

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

        template <typename Packet>
        void channelRead(const DecodedPacket<Packet> &decoded)
        {
            if (connection_.handler_ && !connection_.handler_->handle(decoded.packet)) {
                connection_.handler_->handleGeneric(decoded.bytes);
            }
        }
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
                                     MinecraftVarintLengthEncoder, MinecraftDecoder, MinecraftEncoder>;

    Endpoint &endpoint_;
    EventLoop &event_loop_;
    spdlog::logger &logger_;
    std::string remote_address_;
    Sink sink_;
    Pipeline pipeline_;
    std::shared_ptr<MinecraftSessionHandler> handler_;
    StateRegistry state_{StateRegistry::Login};
    ProtocolVersion protocol_version_{MINIMUM_VERSION};
};

}  // namespace endgate
