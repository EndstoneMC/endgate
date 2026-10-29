#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <exception>
#include <tuple>
#include <utility>

namespace endgate {

/**
 * Marks a handler that receives inbound events.
 *
 * @see io.netty.channel.ChannelInboundHandler
 */
class ChannelInboundHandler {};

/**
 * Marks a handler that intercepts outbound operations.
 *
 * @see io.netty.channel.ChannelOutboundHandler
 */
class ChannelOutboundHandler {};

/**
 * Passes every inbound event to the next handler, so a subclass overrides only the events it cares
 * about. Declaring a member by the event's name hides the pass-through for every message type.
 *
 * @see io.netty.channel.ChannelInboundHandlerAdapter
 */
class ChannelInboundHandlerAdapter : public ChannelInboundHandler {
public:
    template <typename Context>
    void channelActive(Context &ctx)
    {
        ctx.fireChannelActive();
    }

    template <typename Context>
    void channelInactive(Context &ctx)
    {
        ctx.fireChannelInactive();
    }

    template <typename Context, typename Message>
    void channelRead(Context &ctx, Message &&message)
    {
        ctx.fireChannelRead(std::forward<Message>(message));
    }

    template <typename Context>
    void channelReadComplete(Context &ctx)
    {
        ctx.fireChannelReadComplete();
    }

    template <typename Context, typename Event>
    void userEventTriggered(Context &ctx, Event &&event)
    {
        ctx.fireUserEventTriggered(std::forward<Event>(event));
    }

    template <typename Context>
    void exceptionCaught(Context &ctx, const std::exception &cause)
    {
        ctx.fireExceptionCaught(cause);
    }
};

/**
 * Passes every outbound operation to the previous handler.
 *
 * @see io.netty.channel.ChannelOutboundHandlerAdapter
 */
class ChannelOutboundHandlerAdapter : public ChannelOutboundHandler {
public:
    template <typename Context, typename Message>
    void write(Context &ctx, Message &&message)
    {
        ctx.write(std::forward<Message>(message));
    }

    template <typename Context>
    void flush(Context &ctx)
    {
        ctx.flush();
    }

    template <typename Context>
    void close(Context &ctx)
    {
        ctx.close();
    }
};

/**
 * @see io.netty.channel.ChannelDuplexHandler
 */
class ChannelDuplexHandler : public ChannelInboundHandlerAdapter, public ChannelOutboundHandlerAdapter {};

/**
 * Hands a handler its neighbours: fire* passes an event to the next inbound handler, and the outbound
 * operations to the previous outbound one. Each call is resolved at compile time.
 *
 * @see io.netty.channel.ChannelHandlerContext
 */
template <typename Pipeline, std::size_t Index>
class ChannelHandlerContext {
public:
    explicit ChannelHandlerContext(Pipeline &pipeline) : pipeline_(pipeline) {}

    [[nodiscard]] Pipeline &pipeline() const { return pipeline_; }

    void fireChannelActive() { pipeline_.template invokeChannelActive<Pipeline::nextInbound(Index + 1)>(); }
    void fireChannelInactive() { pipeline_.template invokeChannelInactive<Pipeline::nextInbound(Index + 1)>(); }

    template <typename Message>
    void fireChannelRead(Message &&message)
    {
        pipeline_.template invokeChannelRead<Pipeline::nextInbound(Index + 1)>(std::forward<Message>(message));
    }

    void fireChannelReadComplete() { pipeline_.template invokeChannelReadComplete<Pipeline::nextInbound(Index + 1)>(); }

    template <typename Event>
    void fireUserEventTriggered(Event &&event)
    {
        pipeline_.template invokeUserEventTriggered<Pipeline::nextInbound(Index + 1)>(std::forward<Event>(event));
    }

    void fireExceptionCaught(const std::exception &cause)
    {
        pipeline_.template invokeExceptionCaught<Pipeline::nextInbound(Index + 1)>(cause);
    }

    template <typename Message>
    void write(Message &&message)
    {
        pipeline_.template invokeWrite<Pipeline::previousOutbound(Index)>(std::forward<Message>(message));
    }

    void flush() { pipeline_.template invokeFlush<Pipeline::previousOutbound(Index)>(); }
    void close() { pipeline_.template invokeClose<Pipeline::previousOutbound(Index)>(); }

    template <typename Message>
    void writeAndFlush(Message &&message)
    {
        write(std::forward<Message>(message));
        flush();
    }

private:
    Pipeline &pipeline_;
};

/**
 * A statically typed chain of handlers ending in a sink. Each handler declares the message types it
 * reads and writes, and the sink is the head for outbound operations and the tail for inbound events, so
 * a chain wired wrong, or left without a sink for a message, fails to build. Handlers are fixed at
 * construction; one that must wait is gated on a flag. The sink provides channelActive,
 * channelInactive, channelRead, channelReadComplete, userEventTriggered and exceptionCaught for what
 * reaches the tail, and write, flush and close for what reaches the head.
 *
 * @see io.netty.channel.ChannelPipeline
 */
template <typename Sink, typename... Handlers>
class ChannelPipeline {
public:
    static constexpr std::size_t SIZE = sizeof...(Handlers);

    explicit ChannelPipeline(Sink &sink, Handlers... handlers) : sink_(sink), handlers_(std::move(handlers)...) {}
    explicit ChannelPipeline(Sink &sink)
        requires(std::default_initializable<Handlers> && ...)
        : sink_(sink)
    {
    }

    [[nodiscard]] static constexpr std::size_t nextInbound(std::size_t from)
    {
        constexpr std::array<bool, SIZE + 1> inbound{std::derived_from<Handlers, ChannelInboundHandler>..., false};
        while (from < SIZE && !inbound[from]) {
            ++from;
        }
        return from;
    }

    [[nodiscard]] static constexpr std::size_t previousOutbound(std::size_t before)
    {
        constexpr std::array<bool, SIZE + 1> outbound{std::derived_from<Handlers, ChannelOutboundHandler>..., false};
        while (before > 0 && !outbound[before - 1]) {
            --before;
        }
        return before - 1;
    }

    void fireChannelActive() { invokeChannelActive<nextInbound(0)>(); }
    void fireChannelInactive() { invokeChannelInactive<nextInbound(0)>(); }

    template <typename Message>
    void fireChannelRead(Message &&message)
    {
        invokeChannelRead<nextInbound(0)>(std::forward<Message>(message));
    }

    void fireChannelReadComplete() { invokeChannelReadComplete<nextInbound(0)>(); }

    template <typename Event>
    void fireUserEventTriggered(Event &&event)
    {
        invokeUserEventTriggered<nextInbound(0)>(std::forward<Event>(event));
    }

    void fireExceptionCaught(const std::exception &cause) { invokeExceptionCaught<nextInbound(0)>(cause); }

    template <typename Message>
    void write(Message &&message)
    {
        invokeWrite<previousOutbound(SIZE)>(std::forward<Message>(message));
    }

    void flush() { invokeFlush<previousOutbound(SIZE)>(); }
    void close() { invokeClose<previousOutbound(SIZE)>(); }

    template <typename Message>
    void writeAndFlush(Message &&message)
    {
        write(std::forward<Message>(message));
        flush();
    }

    template <typename Handler>
    [[nodiscard]] Handler &get()
    {
        return std::get<Handler>(handlers_);
    }

private:
    template <typename, std::size_t>
    friend class ChannelHandlerContext;

    template <std::size_t Index>
    [[nodiscard]] auto context()
    {
        return ChannelHandlerContext<ChannelPipeline, Index>{*this};
    }

    template <std::size_t Index>
    void invokeChannelActive()
    {
        if constexpr (Index < SIZE) {
            auto ctx = context<Index>();
            try {
                std::get<Index>(handlers_).channelActive(ctx);
            }
            catch (const std::exception &cause) {
                invokeExceptionCaught<Index>(cause);
            }
        }
        else {
            sink_.channelActive();
        }
    }

    template <std::size_t Index>
    void invokeChannelInactive()
    {
        if constexpr (Index < SIZE) {
            auto ctx = context<Index>();
            try {
                std::get<Index>(handlers_).channelInactive(ctx);
            }
            catch (const std::exception &cause) {
                invokeExceptionCaught<Index>(cause);
            }
        }
        else {
            sink_.channelInactive();
        }
    }

    template <std::size_t Index, typename Message>
    void invokeChannelRead(Message &&message)
    {
        if constexpr (Index < SIZE) {
            auto ctx = context<Index>();
            try {
                std::get<Index>(handlers_).channelRead(ctx, std::forward<Message>(message));
            }
            catch (const std::exception &cause) {
                invokeExceptionCaught<Index>(cause);
            }
        }
        else {
            sink_.channelRead(std::forward<Message>(message));
        }
    }

    template <std::size_t Index>
    void invokeChannelReadComplete()
    {
        if constexpr (Index < SIZE) {
            auto ctx = context<Index>();
            try {
                std::get<Index>(handlers_).channelReadComplete(ctx);
            }
            catch (const std::exception &cause) {
                invokeExceptionCaught<Index>(cause);
            }
        }
        else {
            sink_.channelReadComplete();
        }
    }

    template <std::size_t Index, typename Event>
    void invokeUserEventTriggered(Event &&event)
    {
        if constexpr (Index < SIZE) {
            auto ctx = context<Index>();
            try {
                std::get<Index>(handlers_).userEventTriggered(ctx, std::forward<Event>(event));
            }
            catch (const std::exception &cause) {
                invokeExceptionCaught<Index>(cause);
            }
        }
        else {
            sink_.userEventTriggered(std::forward<Event>(event));
        }
    }

    template <std::size_t Index>
    void invokeExceptionCaught(const std::exception &cause)
    {
        if constexpr (Index < SIZE) {
            auto ctx = context<Index>();
            std::get<Index>(handlers_).exceptionCaught(ctx, cause);
        }
        else {
            sink_.exceptionCaught(cause);
        }
    }

    template <std::size_t Index, typename Message>
    void invokeWrite(Message &&message)
    {
        if constexpr (Index < SIZE) {
            auto ctx = context<Index>();
            try {
                std::get<Index>(handlers_).write(ctx, std::forward<Message>(message));
            }
            catch (const std::exception &cause) {
                fireExceptionCaught(cause);
            }
        }
        else {
            sink_.write(std::forward<Message>(message));
        }
    }

    template <std::size_t Index>
    void invokeFlush()
    {
        if constexpr (Index < SIZE) {
            auto ctx = context<Index>();
            try {
                std::get<Index>(handlers_).flush(ctx);
            }
            catch (const std::exception &cause) {
                fireExceptionCaught(cause);
            }
        }
        else {
            sink_.flush();
        }
    }

    template <std::size_t Index>
    void invokeClose()
    {
        if constexpr (Index < SIZE) {
            auto ctx = context<Index>();
            try {
                std::get<Index>(handlers_).close(ctx);
            }
            catch (const std::exception &cause) {
                fireExceptionCaught(cause);
            }
        }
        else {
            sink_.close();
        }
    }

    Sink &sink_;
    std::tuple<Handlers...> handlers_;
};

}  // namespace endgate
