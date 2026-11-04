#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <typeinfo>
#include <utility>

#include <asio/ip/udp.hpp>
#include <spdlog/logger.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/connection/future.h"
#include "endgate/network/eventloop.h"
#include "endgate/network/pipeline.h"
#include "endgate/network/timeout.h"
#include "endgate/object.h"
#include "endgate/protocol/cipher.h"
#include "endgate/protocol/compress.h"
#include "endgate/protocol/entity.h"
#include "endgate/protocol/frame.h"
#include "endgate/protocol/packet.h"
#include "endgate/protocol/state.h"

namespace endgate {

class Endpoint;

[[nodiscard]] inline std::string toString(const asio::ip::udp::endpoint &address)
{
    const auto ip = address.address();
    if (ip.is_v6() && ip.to_v6().is_v4_mapped()) {
        return asio::ip::make_address_v4(asio::ip::v4_mapped, ip.to_v6()).to_string() + ":" +
               std::to_string(address.port());
    }
    return ip.to_string() + ":" + std::to_string(address.port());
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
class MinecraftSessionHandler : public PacketHandlers<RegisteredPackets>, public Object {
public:
    [[nodiscard]] bool isInstanceOf(const std::type_info &target) const override
    {
        return target == typeid(MinecraftSessionHandler) || Object::isInstanceOf(target);
    }

    /**
     * Called before each packet or unknown batch; true drops it.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#beforeHandle
     */
    virtual bool beforeHandle() { return false; }

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
     * Called when the handler becomes the connection's active one.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#activated
     */
    virtual void activated() {}

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftSessionHandler#disconnected
     */
    virtual void disconnected() = 0;
};

template <typename Self, typename Base, typename List>
class HandleEach;

template <typename Self, typename Base>
class HandleEach<Self, Base, TypeList<>> : public Base {
public:
    using Base::handle;
};

template <typename Self, typename Base, typename Packet, typename... Rest>
class HandleEach<Self, Base, TypeList<Packet, Rest...>> : public HandleEach<Self, Base, TypeList<Rest...>> {
public:
    using HandleEach<Self, Base, TypeList<Rest...>>::handle;

    bool handle(const Packet &packet) override { return static_cast<Self *>(this)->handleEach(packet); }
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
    template <typename Message>
    void write(Message message)
    {
        if (event_loop_.inEventLoop()) {
            const auto self = shared_from_this();
            pipeline_.writeAndFlush(std::move(message));
            return;
        }
        event_loop_.execute([self = shared_from_this(), message = std::move(message)]() mutable {
            self->pipeline_.writeAndFlush(std::move(message));
        });
    }

    /**
     * Writes and flushes. The future completes once the transport holds the message where a close
     * cannot lose it. Call on the connection's event loop.
     *
     * @see io.netty.channel.ChannelOutboundInvoker#writeAndFlush
     */
    template <typename Message>
    ChannelFuture writeAndFlush(Message message)
    {
        const auto self = shared_from_this();
        auto promise = newPromise();
        promise_ = promise;
        pipeline_.writeAndFlush(std::move(message));
        std::exchange(promise_, voidPromise()).tryFailure(newClosedChannelException());
        return promise;
    }

    /**
     * @see io.netty.channel.ChannelOutboundInvoker#newPromise
     */
    [[nodiscard]] ChannelPromise newPromise() { return ChannelPromise{shared_from_this()}; }

    /**
     * @see io.netty.channel.ChannelOutboundInvoker#voidPromise
     */
    [[nodiscard]] static ChannelPromise voidPromise() { return {}; }

    /**
     * Writes without flushing.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#delayedWrite
     */
    template <typename Message>
    void delayedWrite(Message message)
    {
        if (event_loop_.inEventLoop()) {
            const auto self = shared_from_this();
            pipeline_.write(std::move(message));
            return;
        }
        event_loop_.execute([self = shared_from_this(), message = std::move(message)]() mutable {
            self->pipeline_.write(std::move(message));
        });
    }

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

    [[nodiscard]] virtual bool isTransportEncrypted() const { return false; }

    /**
     * Call before the connection starts or on its event loop.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#setActiveSessionHandler
     */
    void setSessionHandler(std::shared_ptr<MinecraftSessionHandler> handler);

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#getActiveSessionHandler
     */
    [[nodiscard]] const std::shared_ptr<MinecraftSessionHandler> &getActiveSessionHandler() const { return handler_; }

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
     * Encrypts and decrypts every batch from here on under the key the handshake derived. Call on the
     * connection's event loop.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#enableEncryption
     * @see EncryptedNetworkPeer::enableEncryption
     */
    void enableEncryption(std::span<const std::byte, MinecraftCipher::KEY_SIZE> key);

    void setEntityMap(const EntityMap &map);

    /**
     * Sends a Disconnect with the reason, and the message when there is one, then closes. Call on the
     * connection's event loop.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#closeWith
     */
    void closeWith(DisconnectFailReason reason, const std::string &message = {});

    /**
     * @see io.netty.channel.Channel#eventLoop
     */
    [[nodiscard]] EventLoop &eventLoop() const { return event_loop_; }
    [[nodiscard]] Endpoint &getEndpoint() const { return endpoint_; }

    /**
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#getRemoteAddress
     */
    [[nodiscard]] const std::string &getRemoteAddress() const { return remote_address_; }

    /**
     * What log lines call this connection: its remote address, or "<ip address withheld>" for a player when
     * enable-player-address-logging is off.
     *
     * @see com.velocitypowered.proxy.connection.MinecraftConnection#getAssociation
     */
    [[nodiscard]] std::string getAssociation() const;

    [[nodiscard]] const asio::ip::address &getRemoteIp() const { return remote_ip_; }

protected:
    /**
     * Sends a batch and completes the promise once a close can no longer lose it.
     *
     * @see io.netty.channel.Channel.Unsafe#write
     */
    virtual void send(ByteBuf batch, ChannelPromise promise) = 0;
    virtual void disconnect(bool wait_for_peer) = 0;

    /**
     * @see io.netty.channel.AbstractChannel#newClosedChannelException
     */
    [[nodiscard]] static std::exception_ptr newClosedChannelException()
    {
        return std::make_exception_ptr(std::system_error(std::make_error_code(std::errc::not_connected)));
    }

    void setRemoteAddress(const asio::ip::udp::endpoint &address)
    {
        remote_address_ = toString(address);
        remote_ip_ = address.address();
        if (remote_ip_.is_v6() && remote_ip_.to_v6().is_v4_mapped()) {
            remote_ip_ = asio::ip::make_address_v4(asio::ip::v4_mapped, remote_ip_.to_v6());
        }
    }

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
            const auto handler = connection_.handler_;
            if (handler && !handler->beforeHandle() && !handler->handle(decoded.packet)) {
                handler->handleGeneric(decoded.bytes);
            }
        }
        void channelReadComplete();
        void exceptionCaught(const std::exception_ptr &cause);
        void write(ByteBuf batch);
        void flush() {}
        void close();

    private:
        MinecraftConnection &connection_;
    };

    using Pipeline =
        ChannelPipeline<Sink, MinecraftCipherDecoder, MinecraftCipherEncoder, MinecraftCompressDecoder,
                        MinecraftCompressEncoder, MinecraftVarintFrameDecoder, ReadTimeoutHandler,
                        MinecraftVarintLengthEncoder, EntityRewriteHandler, MinecraftDecoder, MinecraftEncoder>;

    Endpoint &endpoint_;
    EventLoop &event_loop_;
    spdlog::logger &logger_;
    Direction direction_;
    std::string remote_address_;
    asio::ip::address remote_ip_;
    Sink sink_;
    Pipeline pipeline_;
    std::shared_ptr<MinecraftSessionHandler> handler_;
    ChannelPromise promise_;
    StateRegistry state_{StateRegistry::Login};
    bool received_bad_packet_{false};
    ProtocolVersion protocol_version_{MINIMUM_VERSION};
};

}  // namespace endgate
