#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/strand.hpp>
#include <spdlog/logger.h>

#include "endgate/network/buffer.h"
#include "endgate/network/legacy/raknet/RakNetTypes.h"

namespace RakNet {
class RakPeer;
}  // namespace RakNet

namespace endgate {

class ProxyServer;

/**
 * Why a connection ended. RakNet says this with a message id on a packet it would have queued; a
 * channel says it by going inactive.
 *
 * @see com.velocitypowered.proxy.connection.MinecraftConnection#closed
 */
enum class DisconnectReason {
    TimedOut,
    ClosedByPeer,
    ClosedByProxy,
    ConnectionAttemptFailed,
    InvalidPassword,
};

/**
 * One listening socket, the executor its traffic is serialised on, and the connections that arrived
 * on it.
 *
 * Everything that touches a reliability layer, this registry or this socket runs on the strand. The
 * reliability layer has no locking of its own, and asio calls a shared socket unsafe for concurrent
 * asynchronous operations, so the strand is the whole guarantee for both.
 *
 * @see com.velocitypowered.proxy.network.Endpoint
 * @see io.netty.channel.EventLoop
 */
class Endpoint {
public:
    using Executor = asio::strand<asio::io_context::executor_type>;

    Endpoint(ProxyServer &server, asio::io_context &io_context, asio::ip::udp::socket socket);

    /// The peer whose connections arrive here. Both legs of a proxied player live on this one peer:
    /// the player connected to us, and the connection we opened to their backend.
    void setPeer(RakNet::RakPeer *peer) { peer_ = peer; }

    [[nodiscard]] std::string getLocalAddress() const;
    [[nodiscard]] const Executor &getExecutor() const { return strand_; }
    [[nodiscard]] asio::ip::udp::socket &getSocket() { return socket_; }

    /// Where a RakPeer puts a datagram it has decided to send. Detached: the completion owns the bytes,
    /// because SendACKs is reached re-entrantly from inside the read path and must not block there.
    void send(const RakNet::SystemAddress &peer, const char *data, int length);
    void send(const RakNet::SystemAddress &peer, const ByteBuf &bytes);

    /// @see io.netty.channel.ChannelInboundHandler#channelActive
    void channelActive(const RakNet::SystemAddress &peer, const RakNet::RakNetGUID &guid);

    /// The only thing that reaches the chain. Everything else RakNet says is consumed at its own level.
    ///
    /// @see io.netty.channel.ChannelInboundHandler#channelRead
    void channelRead(const RakNet::SystemAddress &peer, const unsigned char *data, unsigned int length);

    /// What the reliability layer reports about a datagram it dropped. Twenty-two of its twenty-three
    /// calls are this, and they are the only surface the ACK/NAK range bound has.
    ///
    /// @see io.netty.channel.ChannelHandler#exceptionCaught
    void onReliabilityLayerNotification(const RakNet::SystemAddress &peer, const char *errorMessage, bool isError);

    /// @see io.netty.channel.ChannelInboundHandler#channelInactive
    void channelInactive(const RakNet::SystemAddress &peer, DisconnectReason reason);

    void close();

    [[nodiscard]] static asio::ip::udp::endpoint toAsio(const RakNet::SystemAddress &address);
    [[nodiscard]] static RakNet::SystemAddress fromAsio(const asio::ip::udp::endpoint &endpoint);

private:
    /// The other leg of a proxied player, keyed by address. A player maps to its backend and the
    /// backend maps back, so a datagram arriving on either leg knows where it is going.
    ///
    /// @see com.velocitypowered.proxy.connection.backend.VelocityServerConnection
    std::unordered_map<std::string, std::string> counterpart_;
    std::unordered_map<std::string, RakNet::SystemAddress> addresses_;
    std::string awaiting_backend_;

    ProxyServer &server_;
    spdlog::logger &logger_;
    RakNet::RakPeer *peer_{nullptr};
    asio::ip::udp::socket socket_;
    Executor strand_;
};

}  // namespace endgate
