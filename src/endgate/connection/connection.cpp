#include "endgate/connection/connection.h"

#include <chrono>
#include <utility>
#include <variant>

#include "endgate/config.h"
#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"
#include "endgate/server.h"

namespace endgate {

MinecraftConnection::MinecraftConnection(Endpoint &endpoint, std::string remote_address, Direction direction)
    : endpoint_(endpoint), event_loop_(endpoint.getEventLoop()), logger_(LogManager::getLogger(*this)),
      direction_(direction), remote_address_(std::move(remote_address)), sink_(*this),
      pipeline_(sink_, MinecraftCipherDecoder{}, MinecraftCipherEncoder{}, MinecraftCompressDecoder{},
                MinecraftCompressEncoder{endpoint.getServer().getConfiguration().getCompressionLevel()},
                MinecraftVarintFrameDecoder{},
                ReadTimeoutHandler{event_loop_,
                                   std::chrono::milliseconds{endpoint.getServer().getConfiguration().getReadTimeout()}},
                MinecraftVarintLengthEncoder{}, EntityRewriteHandler{}, MinecraftDecoder{direction}, MinecraftEncoder{})
{
}

std::string MinecraftConnection::getAssociation() const
{
    if (direction_ == Direction::Serverbound &&
        !endpoint_.getServer().getConfiguration().isPlayerAddressLoggingEnabled()) {
        return "<ip address withheld>";
    }
    return remote_address_;
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
    if (const auto activated = handler_) {
        activated->activated();
    }
}

void MinecraftConnection::setState(StateRegistry state)
{
    state_ = state;
    pipeline_.get<MinecraftVarintFrameDecoder>().setEnabled(true);
    pipeline_.get<MinecraftVarintLengthEncoder>().setEnabled(true);
    auto &decoder = pipeline_.get<MinecraftDecoder>();
    decoder.setEnabled(true);
    decoder.setState(state);
}

void MinecraftConnection::setProtocolVersion(ProtocolVersion version)
{
    protocol_version_ = version;
    pipeline_.get<EntityRewriteHandler>().setProtocolVersion(version);
    pipeline_.get<MinecraftDecoder>().setProtocolVersion(version);
    pipeline_.get<MinecraftEncoder>().setProtocolVersion(version);
}

void MinecraftConnection::enableEncryption(std::span<const std::byte, MinecraftCipher::KEY_SIZE> key)
{
    pipeline_.get<MinecraftCipherDecoder>().enableEncryption(key);
    pipeline_.get<MinecraftCipherEncoder>().enableEncryption(key);
}

void MinecraftConnection::setEntityMap(const EntityMap &map)
{
    pipeline_.get<EntityRewriteHandler>().setEntityMap(map);
}

void MinecraftConnection::closeWith(DisconnectFailReason reason, const std::string &message)
{
    visitProtocolVersion(protocol_version_.getProtocol(), [&]<int Version>() {
        bp::DisconnectPacket_<Version> packet;
        packet.reason = static_cast<decltype(packet.reason)>(reason);
        if (message.empty()) {
            packet.messages = std::monostate{};
        }
        else {
            packet.messages = bp::DisconnectPacketMessages{.message = message, .filtered_message = message};
        }
        writeAndFlush(std::move(packet)).addListener(ChannelFutureListener::CLOSE);
        return true;
    });
}

void MinecraftConnection::setCompressionThreshold(std::uint16_t threshold, PacketCompressionAlgorithm algorithm)
{
    pipeline_.get<MinecraftCompressDecoder>().setCompressionEnabled(true, algorithm);
    pipeline_.get<MinecraftCompressEncoder>().setCompressionEnabled(true, algorithm, threshold);
}

void MinecraftConnection::channelActive()
{
    pipeline_.fireChannelActive();
}

void MinecraftConnection::channelRead(ByteBufView batch)
{
    pipeline_.fireChannelRead(batch);
    if (std::exchange(received_bad_packet_, false)) {
        closeWith(DisconnectFailReason::BadPacket);
    }
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
    if (const auto handler = connection_.handler_) {
        handler->connected();
    }
}

void MinecraftConnection::Sink::channelRead(ByteBufView batch)
{
    if (const auto handler = connection_.handler_; handler && !handler->beforeHandle()) {
        handler->handleUnknown(batch);
    }
}

void MinecraftConnection::Sink::channelReadComplete()
{
    if (const auto handler = connection_.handler_) {
        handler->readCompleted();
    }
}

void MinecraftConnection::Sink::channelInactive()
{
    if (const auto handler = std::exchange(connection_.handler_, nullptr)) {
        handler->disconnected();
    }
    connection_.endpoint_.unregisterConnection(connection_.shared_from_this());
}

void MinecraftConnection::Sink::exceptionCaught(const std::exception_ptr &cause)
{
    try {
        std::rethrow_exception(cause);
    }
    catch (const ReadTimeoutException &) {
        connection_.logger_.error("{}: read timed out", connection_.getAssociation());
    }
    catch (const DecoderException &decoder_cause) {
        if (connection_.endpoint_.getServer().getConfiguration().isPacketDecodeLogging()) {
            connection_.logger_.error("{}: invalid packet received: {}", connection_.getAssociation(),
                                      decoder_cause.what());
        }
        else {
            connection_.logger_.warn(
                "Invalid packet received. Turn on packet-decode-logging under [debug] in endgate.toml to see more.");
        }
        connection_.received_bad_packet_ = true;
    }
    catch (const std::exception &other_cause) {
        connection_.logger_.error("{}: exception encountered: {}", connection_.getAssociation(), other_cause.what());
        connection_.disconnect(true);
    }
}

void MinecraftConnection::Sink::write(ByteBuf batch)
{
    connection_.send(std::move(batch), std::exchange(connection_.promise_, voidPromise()));
}

void MinecraftConnection::Sink::close()
{
    connection_.disconnect(true);
}

}  // namespace endgate
