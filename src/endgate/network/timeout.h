#pragma once

#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>

#include "endgate/network/eventloop.h"
#include "endgate/network/pipeline.h"

namespace endgate {

/**
 * @see io.netty.handler.timeout.ReadTimeoutException
 */
class ReadTimeoutException : public std::runtime_error {
public:
    ReadTimeoutException() : std::runtime_error("read timeout") {}
};

/**
 * Raises a ReadTimeoutException and closes the connection when nothing has been read for the timeout.
 * A zero or negative timeout turns it off.
 *
 * @see io.netty.handler.timeout.ReadTimeoutHandler
 */
class ReadTimeoutHandler : public ChannelInboundHandlerAdapter {
public:
    using Clock = std::chrono::steady_clock;

    ReadTimeoutHandler(EventLoop &event_loop, std::chrono::milliseconds timeout)
        : event_loop_(event_loop), timeout_(timeout)
    {
    }

    template <typename Context>
    void channelActive(Context &ctx)
    {
        initialize(ctx);
        ctx.fireChannelActive();
    }

    template <typename Context>
    void channelInactive(Context &ctx)
    {
        destroy();
        ctx.fireChannelInactive();
    }

    template <typename Context, typename Message>
    void channelRead(Context &ctx, Message &&message)
    {
        reading_ = true;
        ctx.fireChannelRead(std::forward<Message>(message));
    }

    template <typename Context>
    void channelReadComplete(Context &ctx)
    {
        if (reading_) {
            last_read_time_ = Clock::now();
            reading_ = false;
        }
        ctx.fireChannelReadComplete();
    }

private:
    template <typename Context>
    void initialize(Context &ctx)
    {
        if (timeout_.count() <= 0 || task_) {
            return;
        }
        last_read_time_ = Clock::now();
        schedule(ctx, timeout_);
    }

    void destroy()
    {
        if (task_) {
            task_->cancel();
            task_.reset();
        }
    }

    template <typename Context>
    void schedule(Context &ctx, Clock::duration delay)
    {
        task_ = event_loop_.schedule([this, ctx]() mutable { run(ctx); }, delay);
    }

    template <typename Context>
    void run(Context &ctx)
    {
        auto next_delay = std::chrono::duration_cast<Clock::duration>(timeout_);
        if (!reading_) {
            next_delay -= Clock::now() - last_read_time_;
        }
        if (next_delay.count() > 0) {
            schedule(ctx, next_delay);
            return;
        }
        task_.reset();
        ctx.fireExceptionCaught(ReadTimeoutException{});
        ctx.close();
    }

    EventLoop &event_loop_;
    std::chrono::milliseconds timeout_;
    std::shared_ptr<ScheduledFuture> task_;
    Clock::time_point last_read_time_;
    bool reading_{false};
};

}  // namespace endgate
