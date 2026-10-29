#include "endgate/connection/connection.h"

#include <utility>
#include <vector>

#include <asio/post.hpp>

#include "endgate/network/endpoint.h"

namespace endgate {

MinecraftConnection::MinecraftConnection(Endpoint &endpoint, asio::io_context &io_context, std::string remote_address)
    : endpoint_(endpoint), strand_(asio::make_strand(io_context)), remote_address_(std::move(remote_address))
{
}

void MinecraftConnection::write(ByteBuf batch)
{
    asio::post(strand_,
               [self = shared_from_this(), batch = std::move(batch)]() mutable { self->send(std::move(batch)); });
}

void MinecraftConnection::close()
{
    asio::post(strand_, [self = shared_from_this()] { self->disconnect(true); });
}

void MinecraftConnection::shutdown()
{
    asio::post(strand_, [self = shared_from_this()] { self->disconnect(false); });
}

void MinecraftConnection::receive(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender)
{
    asio::post(strand_, [self = shared_from_this(), datagram = std::vector(datagram.begin(), datagram.end()), sender] {
        self->handleDatagram(datagram, sender);
    });
}

void MinecraftConnection::setSessionHandler(std::shared_ptr<MinecraftSessionHandler> handler)
{
    handler_ = std::move(handler);
}

void MinecraftConnection::channelActive()
{
    endpoint_.activated(shared_from_this());
    if (handler_) {
        handler_->connected();
    }
}

void MinecraftConnection::channelRead(ByteBuf batch)
{
    if (handler_) {
        handler_->handleUnknown(std::move(batch));
    }
}

void MinecraftConnection::channelInactive()
{
    if (const auto handler = std::exchange(handler_, nullptr)) {
        handler->disconnected();
    }
    endpoint_.unregisterConnection(shared_from_this());
}

}  // namespace endgate
