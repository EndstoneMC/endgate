#include "endgate/connection/connection.h"

#include <utility>

#include "endgate/config.h"
#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"
#include "endgate/server.h"

namespace endgate {

MinecraftConnection::MinecraftConnection(Endpoint &endpoint, std::string remote_address)
    : endpoint_(endpoint), event_loop_(endpoint.getEventLoop()),
      logger_(LogManager::getLogger("endgate::MinecraftConnection")), remote_address_(std::move(remote_address)),
      sink_(*this), pipeline_(sink_)
{
}

void MinecraftConnection::write(ByteBuf batch)
{
    if (event_loop_.inEventLoop()) {
        const auto self = shared_from_this();
        pipeline_.writeAndFlush(std::move(batch));
        return;
    }
    event_loop_.execute([self = shared_from_this(), batch = std::move(batch)]() mutable {
        self->pipeline_.writeAndFlush(std::move(batch));
    });
}

void MinecraftConnection::delayedWrite(ByteBuf batch)
{
    if (event_loop_.inEventLoop()) {
        const auto self = shared_from_this();
        pipeline_.write(std::move(batch));
        return;
    }
    event_loop_.execute(
        [self = shared_from_this(), batch = std::move(batch)]() mutable { self->pipeline_.write(std::move(batch)); });
}

void MinecraftConnection::flush()
{
    if (event_loop_.inEventLoop()) {
        const auto self = shared_from_this();
        pipeline_.flush();
        return;
    }
    event_loop_.execute([self = shared_from_this()] { self->pipeline_.flush(); });
}

void MinecraftConnection::close()
{
    if (event_loop_.inEventLoop()) {
        const auto self = shared_from_this();
        pipeline_.close();
        return;
    }
    event_loop_.execute([self = shared_from_this()] { self->pipeline_.close(); });
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
    pipeline_.fireChannelActive();
}

void MinecraftConnection::channelRead(ByteBufView batch)
{
    pipeline_.fireChannelRead(batch);
}

void MinecraftConnection::channelReadComplete()
{
    pipeline_.fireChannelReadComplete();
}

void MinecraftConnection::channelInactive()
{
    pipeline_.fireChannelInactive();
}

void MinecraftConnection::Sink::channelActive()
{
    connection_.endpoint_.activated(connection_.shared_from_this());
    if (connection_.handler_) {
        connection_.handler_->connected();
    }
}

void MinecraftConnection::Sink::channelRead(ByteBufView batch)
{
    if (connection_.handler_) {
        connection_.handler_->handleUnknown(batch);
    }
}

void MinecraftConnection::Sink::channelReadComplete()
{
    if (connection_.handler_) {
        connection_.handler_->readCompleted();
    }
}

void MinecraftConnection::Sink::channelInactive()
{
    if (const auto handler = std::exchange(connection_.handler_, nullptr)) {
        handler->disconnected();
    }
    connection_.endpoint_.unregisterConnection(connection_.shared_from_this());
}

void MinecraftConnection::Sink::exceptionCaught(const std::exception &cause)
{
    if (dynamic_cast<const DecoderException *>(&cause) == nullptr) {
        connection_.logger_.error("{}: exception encountered: {}", connection_.remote_address_, cause.what());
    }
    else if (connection_.endpoint_.getServer().getConfiguration().isPacketDecodeLogging()) {
        connection_.logger_.error("{}: invalid packet received: {}", connection_.remote_address_, cause.what());
    }
    else {
        connection_.logger_.warn(
            "Invalid packet received. Turn on packet-decode-logging under [debug] in endgate.toml to see more.");
    }
    connection_.disconnect(true);
}

void MinecraftConnection::Sink::write(ByteBuf batch)
{
    connection_.send(std::move(batch));
}

void MinecraftConnection::Sink::close()
{
    connection_.disconnect(true);
}

}  // namespace endgate
