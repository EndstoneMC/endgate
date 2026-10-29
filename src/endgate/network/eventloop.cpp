#include "endgate/network/eventloop.h"

#include <format>
#include <utility>

#include "endgate/thread.h"

namespace endgate {

EventLoop::EventLoop(std::string name)
    : io_context_(1), work_guard_(asio::make_work_guard(io_context_)), thread_([this, name = std::move(name)] {
          Thread::setName(name);
          io_context_.run();
      })
{
}

ScheduledFuture::ScheduledFuture(EventLoop &event_loop) : event_loop_(event_loop), timer_(event_loop.getExecutor()) {}

void ScheduledFuture::cancel()
{
    event_loop_.execute([self = shared_from_this()] { self->timer_.cancel(); });
}

EventLoop::~EventLoop()
{
    shutdownGracefully();
    if (inEventLoop()) {
        thread_.detach();
        return;
    }
    awaitTermination();
}

void EventLoop::shutdownGracefully()
{
    work_guard_.reset();
}

void EventLoop::awaitTermination()
{
    if (thread_.joinable() && !inEventLoop()) {
        thread_.join();
    }
}

EventLoopGroup::EventLoopGroup(std::size_t threads, std::string_view name)
{
    for (std::size_t i = 0; i < threads; ++i) {
        loops_.emplace_back(std::format("{} #{}", name, i));
    }
}

EventLoop &EventLoopGroup::next()
{
    return loops_[next_.fetch_add(1) % loops_.size()];
}

void EventLoopGroup::shutdownGracefully()
{
    for (auto &loop : loops_) {
        loop.shutdownGracefully();
    }
}

void EventLoopGroup::awaitTermination()
{
    for (auto &loop : loops_) {
        loop.awaitTermination();
    }
}

}  // namespace endgate
