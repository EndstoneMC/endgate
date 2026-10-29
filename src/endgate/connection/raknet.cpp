#include "endgate/connection/raknet.h"

#include <array>
#include <tuple>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>
#include <netinet/in.h>
#include <sys/socket.h>

#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"

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
    : MinecraftConnection(endpoint, endpoint.getIoContext(), toString(address)), logger_(LogManager::getLogger()),
      endpoint_socket_(endpoint.getSocket()), socket_(getExecutor()), address_(address), timer_(getExecutor()),
      connection_(*this, {.guid = guid}, address, request, system_index, now)
{
}

RakNetConnection::RakNetConnection(Endpoint &endpoint, const raknet::SystemAddress &address, std::uint64_t guid,
                                   raknet::TimePoint now)
    : MinecraftConnection(endpoint, endpoint.getIoContext(), toString(address)), logger_(LogManager::getLogger()),
      socket_(getExecutor()), address_(address), timer_(getExecutor()), connection_(*this, {.guid = guid}, address, now)
{
}

void RakNetConnection::connect()
{
    asio::post(getExecutor(), [self = std::static_pointer_cast<RakNetConnection>(shared_from_this())] {
        if (self->connection_.isClosed()) {
            return;
        }
        asio::error_code ec;
        if (self->socket_.open(self->address_.protocol(), ec) || self->socket_.connect(self->address_, ec) ||
            self->socket_.non_blocking(true, ec)) {
            self->logger_.warn("Can't open a socket to backend {}: {}", self->getRemoteAddress(), ec.message());
            self->connection_.close(false, raknet::Clock::now());
            return;
        }
        asio::co_spawn(self->getExecutor(), read(self), asio::detached);
        self->connection_.handleTimeout(raknet::Clock::now());
        self->arm();
    });
}

asio::awaitable<void> RakNetConnection::read(std::shared_ptr<RakNetConnection> self)
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
    connection_.handleDatagram(datagram, raknet::Clock::now());
    arm();
}

void RakNetConnection::send(LegacyByteBuf batch)
{
    if (!active_) {
        logger_.debug("Dropping {} bytes for {}: not connected", batch.length(), getRemoteAddress());
        return;
    }
    static const auto header = LegacyByteBuf::copyBuffer(std::array{BATCH_HEADER});
    auto message = header;
    message.appendToChain(std::move(batch));
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

void RakNetConnection::sendDatagram(std::span<const std::byte> datagram)
{
    asio::error_code ignored;
    if (endpoint_socket_) {
        endpoint_socket_->send_to(asio::buffer(datagram), address_, 0, ignored);
        return;
    }
    socket_.send(asio::buffer(datagram), 0, ignored);
}

bool RakNetConnection::sendMtuProbe(std::span<const std::byte> datagram)
{
    if (endpoint_socket_) {
        sendDatagram(datagram);
        return true;
    }
    asio::error_code ec;
    setDontFragment(socket_, true);
    socket_.send(asio::buffer(datagram), 0, ec);
    setDontFragment(socket_, false);
    return ec != asio::error::message_size;
}

void RakNetConnection::onConnected()
{
    active_ = true;
    channelActive();
}

void RakNetConnection::onMessage(LegacyByteBuf message)
{
    std::array<std::byte, 1> first{};
    message.copyTo(first);
    if (message.empty() || first.front() != BATCH_HEADER) {
        logger_.debug("Dropping {} bytes from {}: not a batch", message.length(), getRemoteAddress());
        return;
    }
    channelRead(message.retainedSlice(1, message.length() - 1));
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
    logger_.debug("{} ended: {}", getRemoteAddress(), raknet::toString(reason));
    channelInactive();
}

void RakNetConnection::onReliabilityLayerNotification(std::string_view message, bool is_error)
{
    if (is_error) {
        logger_.warn("Malformed datagram from {}: {}", getRemoteAddress(), message);
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
        self->arm();
    });
}

}  // namespace endgate
