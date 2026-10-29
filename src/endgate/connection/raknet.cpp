#include "endgate/connection/raknet.h"

#include <array>
#include <tuple>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>
#include <netinet/in.h>
#include <sys/socket.h>

#include "endgate/awaitable.h"
#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"
#include "endgate/server.h"

namespace endgate {

namespace {

constexpr std::byte BATCH_HEADER{0xFE};

void setDontFragment(asio::ip::udp::socket &socket, bool dont_fragment)
{
    asio::error_code ec;
    if (socket.local_endpoint(ec).protocol() == asio::ip::udp::v4()) {
        const int value = dont_fragment ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
        static_cast<void>(::setsockopt(socket.native_handle(), IPPROTO_IP, IP_MTU_DISCOVER, &value, sizeof(value)));
        return;
    }
    const int value = dont_fragment ? IPV6_PMTUDISC_DO : IPV6_PMTUDISC_DONT;
    static_cast<void>(::setsockopt(socket.native_handle(), IPPROTO_IPV6, IPV6_MTU_DISCOVER, &value, sizeof(value)));
}

}  // namespace

RakNetConnection::RakNetConnection(Endpoint &endpoint, const raknet::SystemAddress &address,
                                   const raknet::OpenConnectionRequest2 &request, std::uint64_t guid,
                                   std::uint16_t system_index, raknet::TimePoint now)
    : MinecraftConnection(endpoint, toString(address)), logger_(LogManager::getLogger("RakNetConnection")),
      endpoint_socket_(endpoint.getSocket()), socket_(eventLoop().getExecutor()), address_(address),
      timer_(eventLoop().getExecutor()), connection_(*this, {.guid = guid}, address, request, system_index, now)
{
}

RakNetConnection::RakNetConnection(Endpoint &endpoint, const raknet::SystemAddress &address, std::uint64_t guid,
                                   raknet::TimePoint now)
    : MinecraftConnection(endpoint, toString(address)), logger_(LogManager::getLogger("RakNetConnection")),
      socket_(eventLoop().getExecutor()), address_(address), timer_(eventLoop().getExecutor()),
      connection_(*this, {.guid = guid}, address, now)
{
}

void RakNetConnection::connect()
{
    if (connection_.isClosed()) {
        return;
    }
    asio::error_code ec;
    if (socket_.open(address_.protocol(), ec) || socket_.connect(address_, ec) || socket_.non_blocking(true, ec)) {
        logger_.error("{}: unable to connect: {}", getRemoteAddress(), ec.message());
        connection_.close(false, raknet::Clock::now());
        return;
    }
    asio::co_spawn(eventLoop().getExecutor(), read(std::static_pointer_cast<RakNetConnection>(shared_from_this())),
                   asio::detached);
    connection_.handleTimeout(raknet::Clock::now());
    arm();
}

Awaitable<void> RakNetConnection::read(std::shared_ptr<RakNetConnection> self)
{
    std::array<std::byte, raknet::MAXIMUM_MTU_SIZE> buffer{};
    for (;;) {
        const auto [ec, size] =
            co_await self->socket_.async_receive(asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted || !self->socket_.is_open()) {
            co_return;
        }
        if (ec || size == 0) {
            continue;
        }
        self->handleDatagram(std::span{buffer}.first(size), self->address_);
    }
}

void RakNetConnection::handleDatagram(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &)
{
    if (datagram.empty() || connection_.isClosed()) {
        return;
    }
    connection_.handleDatagram(Unpooled::wrappedBuffer(datagram), raknet::Clock::now());
    channelReadComplete();
    arm();
}

void RakNetConnection::send(ByteBuf batch)
{
    if (!active_) {
        logger_.debug("{}: dropping {} bytes, not connected", getRemoteAddress(), batch.readableBytes());
        return;
    }
    auto message = Unpooled::buffer(1 + batch.readableBytes());
    message.writeByte(std::to_integer<std::uint8_t>(BATCH_HEADER));
    message.writeBytes(batch.nioBuffer());
    connection_.send(std::move(message), raknet::PacketPriority::High, raknet::PacketReliability::ReliableOrdered, 0,
                     raknet::Clock::now());
    arm();
}

void RakNetConnection::disconnect(bool wait_for_peer)
{
    const auto now = raknet::Clock::now();
    connection_.close(true, now);
    if (!wait_for_peer) {
        connection_.close(false, now);
    }
    arm();
}

void RakNetConnection::sendDatagram(ByteBufView datagram)
{
    asio::error_code ignored;
    if (endpoint_socket_) {
        endpoint_socket_->send_to(asio::buffer(datagram.nioBuffer()), address_, 0, ignored);
        return;
    }
    socket_.send(asio::buffer(datagram.nioBuffer()), 0, ignored);
}

bool RakNetConnection::sendMtuProbe(ByteBufView datagram)
{
    if (endpoint_socket_) {
        sendDatagram(datagram);
        return true;
    }
    asio::error_code ec;
    setDontFragment(socket_, true);
    socket_.send(asio::buffer(datagram.nioBuffer()), 0, ec);
    setDontFragment(socket_, false);
    return ec != asio::error::message_size;
}

void RakNetConnection::onConnected()
{
    active_ = true;
    channelActive();
}

void RakNetConnection::onMessage(ByteBufView message)
{
    if (!message.isReadable() ||
        message.getUnsignedByte(message.readerIndex()) != std::to_integer<std::uint8_t>(BATCH_HEADER)) {
        logger_.debug("{}: dropping {} bytes, not a batch", getRemoteAddress(), message.readableBytes());
        return;
    }
    message.skipBytes(1);
    channelRead(message);
}

void RakNetConnection::onDisconnected(raknet::DisconnectReason reason)
{
    active_ = false;
    timer_.cancel();
    armed_.reset();
    if (socket_.is_open()) {
        asio::error_code ignored;
        std::ignore = socket_.close(ignored);
    }
    logger_.debug("{}: connection ended: {}", getRemoteAddress(), raknet::toString(reason));
    channelInactive();
}

void RakNetConnection::onReliabilityLayerNotification(std::string_view message, bool is_error)
{
    if (is_error) {
        if (getEndpoint().getServer().getConfiguration().isPacketDecodeLogging()) {
            logger_.error("{}: invalid packet received: {}", getRemoteAddress(), message);
        }
        else {
            logger_.warn("Invalid packet received. Turn on packet-decode-logging under [debug] in endgate.toml to see "
                         "more.");
        }
    }
}

void RakNetConnection::arm()
{
    const auto next = connection_.nextTimeout();
    if (!next) {
        if (armed_) {
            timer_.cancel();
            armed_.reset();
        }
        return;
    }
    if (armed_ && *armed_ <= *next) {
        return;
    }
    armed_ = next;
    timer_.expires_at(*next);
    timer_.async_wait([weak = std::weak_ptr(std::static_pointer_cast<RakNetConnection>(shared_from_this()))](
                          const asio::error_code &ec) {
        const auto self = weak.lock();
        if (ec || !self) {
            return;
        }
        self->armed_.reset();
        self->connection_.handleTimeout(raknet::Clock::now());
        self->channelReadComplete();
        self->arm();
    });
}

}  // namespace endgate
