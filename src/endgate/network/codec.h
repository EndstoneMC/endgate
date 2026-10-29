#pragma once

#include <concepts>
#include <exception>
#include <stdexcept>
#include <utility>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/pipeline.h"

namespace endgate {

/**
 * @see io.netty.handler.codec.DecoderException
 */
class DecoderException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @see io.netty.handler.codec.CorruptedFrameException
 */
class CorruptedFrameException : public DecoderException {
public:
    using DecoderException::DecoderException;
};

/**
 * @see io.netty.handler.codec.EncoderException
 */
class EncoderException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * Turns the bytes of each read into messages. The subclass's decode(ctx, in) consumes from in and calls
 * fireChannelRead for each message it produces, and what it leaves unread waits for the next read.
 *
 * @see io.netty.handler.codec.ByteToMessageDecoder
 */
class ByteToMessageDecoder : public ChannelInboundHandlerAdapter {
public:
    template <typename Self, typename Context>
    void channelRead(this Self &self, Context &ctx, ByteBufView in)
    {
        const bool cumulated = self.cumulation_.isReadable();
        if (cumulated) {
            self.cumulation_.writeBytes(in);
            in = self.cumulation_.slice();
        }
        try {
            while (in.isReadable()) {
                const auto readable = in.readableBytes();
                self.decode(ctx, in);
                if (in.readableBytes() == readable) {
                    break;
                }
            }
        }
        catch (const DecoderException &) {
            self.cumulation_.clear();
            throw;
        }
        catch (const std::exception &cause) {
            self.cumulation_.clear();
            throw DecoderException(cause.what());
        }
        if (cumulated) {
            self.cumulation_.skipBytes(in.readerIndex()).discardReadBytes();
        }
        else if (in.isReadable()) {
            self.cumulation_.writeBytes(in);
        }
    }

    template <typename Context>
    void channelInactive(Context &ctx)
    {
        cumulation_.clear();
        ctx.fireChannelInactive();
    }

private:
    ByteBuf cumulation_;
};

/**
 * Turns each message into zero or more others. The subclass's decode(ctx, message) calls fireChannelRead
 * for each; a message type decode does not take passes on unchanged.
 *
 * @see io.netty.handler.codec.MessageToMessageDecoder
 */
class MessageToMessageDecoder : public ChannelInboundHandlerAdapter {
public:
    template <typename Self, typename Context, typename Message>
    void channelRead(this Self &self, Context &ctx, Message &&message)
    {
        if constexpr (requires { self.decode(ctx, std::forward<Message>(message)); }) {
            try {
                self.decode(ctx, std::forward<Message>(message));
            }
            catch (const DecoderException &) {
                throw;
            }
            catch (const std::exception &cause) {
                throw DecoderException(cause.what());
            }
        }
        else {
            ctx.fireChannelRead(std::forward<Message>(message));
        }
    }
};

/**
 * Encodes each message into a buffer and writes it. The subclass's encode(ctx, message, out) fills out;
 * a message type encode does not take passes on unchanged.
 *
 * @see io.netty.handler.codec.MessageToByteEncoder
 */
class MessageToByteEncoder : public ChannelOutboundHandlerAdapter {
public:
    template <typename Self, typename Context, typename Message>
    void write(this Self &self, Context &ctx, Message &&message)
    {
        if constexpr (requires(ByteBuf &out) { self.encode(ctx, message, out); }) {
            auto out = Unpooled::buffer();
            try {
                self.encode(ctx, message, out);
            }
            catch (const EncoderException &) {
                throw;
            }
            catch (const std::exception &cause) {
                throw EncoderException(cause.what());
            }
            ctx.write(std::move(out));
        }
        else {
            ctx.write(std::forward<Message>(message));
        }
    }
};

}  // namespace endgate
