#include "endgate/connection/connection.h"

#include <utility>

#include "endgate/network/endpoint.h"

namespace endgate {

MinecraftConnection::MinecraftConnection(Endpoint &endpoint, std::string remote_address)
    : endpoint_(endpoint), event_loop_(endpoint.getEventLoop()), remote_address_(std::move(remote_address))
{
}

void MinecraftConnection::write(ByteBuf batch)
{
    if (event_loop_.inEventLoop()) {
        const auto self = shared_from_this();
        send(std::move(batch));
        return;
    }
    event_loop_.execute(
        [self = shared_from_this(), batch = std::move(batch)]() mutable { self->send(std::move(batch)); });
}

void MinecraftConnection::close()
{
    if (event_loop_.inEventLoop()) {
        const auto self = shared_from_this();
        disconnect(true);
        return;
    }
    event_loop_.execute([self = shared_from_this()] { self->disconnect(true); });
}

void MinecraftConnection::shutdown()
{
    if (event_loop_.inEventLoop()) {
        const auto self = shared_from_this();
        disconnect(false);
        return;
    }
    event_loop_.execute([self = shared_from_this()] { self->disconnect(false); });
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
