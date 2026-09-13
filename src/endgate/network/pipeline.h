#pragma once

#include <cstddef>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>

namespace endgate {

/**
 * Hands a stage its neighbours. fireRead may be called any number of times for one input, which is how
 * one batch becomes many packets without a poll.
 *
 * @see io.netty.channel.ChannelHandlerContext
 */
template <typename Pipeline, std::size_t Index>
class HandlerContext {
public:
    explicit HandlerContext(Pipeline &pipeline) : pipeline_(pipeline) {}

    template <typename Message>
    void fireRead(Message &&message)
    {
        pipeline_.template readAt<Index + 1>(std::forward<Message>(message));
    }

    template <typename Message>
    void write(Message &&message)
    {
        pipeline_.template writeAt<Index>(std::forward<Message>(message));
    }

    void fireExceptionCaught(std::exception_ptr error)
    {
        pipeline_.exceptionCaught(error);
    }

private:
    Pipeline &pipeline_;
};

/**
 * A statically typed chain of stages. Each stage declares what it reads and what it emits, so a chain
 * whose types do not line up fails to compile rather than failing at runtime.
 *
 * @see io.netty.channel.ChannelPipeline
 * @see wangle::Pipeline
 */
template <typename Sink, typename... Handlers>
class Pipeline {
public:
    explicit Pipeline(Sink sink, Handlers... handlers)
        : sink_(std::move(sink)), handlers_(std::move(handlers)...)
    {
    }

    template <typename Message>
    void read(Message &&message)
    {
        readAt<0>(std::forward<Message>(message));
    }

    template <typename Message>
    void write(Message &&message)
    {
        writeAt<sizeof...(Handlers)>(std::forward<Message>(message));
    }

    template <std::size_t Index, typename Message>
    void readAt(Message &&message)
    {
        if constexpr (Index < sizeof...(Handlers)) {
            HandlerContext<Pipeline, Index> context{*this};
            std::get<Index>(handlers_).read(context, std::forward<Message>(message));
        }
        else {
            sink_.read(std::forward<Message>(message));
        }
    }

    template <std::size_t Index, typename Message>
    void writeAt(Message &&message)
    {
        if constexpr (Index > 0) {
            HandlerContext<Pipeline, Index - 1> context{*this};
            std::get<Index - 1>(handlers_).write(context, std::forward<Message>(message));
        }
        else {
            sink_.write(std::forward<Message>(message));
        }
    }

    void exceptionCaught(std::exception_ptr error)
    {
        sink_.exceptionCaught(error);
    }

    template <std::size_t Index>
    [[nodiscard]] auto &handler()
    {
        return std::get<Index>(handlers_);
    }

private:
    Sink sink_;
    std::tuple<Handlers...> handlers_;
};

}  // namespace endgate
