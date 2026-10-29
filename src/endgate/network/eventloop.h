#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <thread>
#include <utility>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

namespace endgate {

/**
 * Handles all the I/O operations of the channels registered to it with one thread.
 *
 * @see io.netty.channel.EventLoop
 * @see io.netty.channel.SingleThreadIoEventLoop
 */
class EventLoop {
public:
    using Executor = asio::io_context::executor_type;

    EventLoop();
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;
    EventLoop(EventLoop &&) = delete;
    EventLoop &operator=(EventLoop &&) = delete;
    ~EventLoop();

    /**
     * Returns true if the calling thread is the thread that runs this loop.
     *
     * @see io.netty.util.concurrent.EventExecutor#inEventLoop
     */
    [[nodiscard]] bool inEventLoop() const { return getExecutor().running_in_this_thread(); }

    /**
     * Runs the task on the loop's thread, after the current task returns.
     *
     * @see java.util.concurrent.Executor#execute
     */
    template <typename F>
    void execute(F &&task)
    {
        asio::post(io_context_, std::forward<F>(task));
    }

    [[nodiscard]] Executor getExecutor() const { return io_context_.get_executor(); }
    [[nodiscard]] asio::io_context &getIoContext() { return io_context_; }

    /**
     * @see io.netty.util.concurrent.EventExecutorGroup#shutdownGracefully
     */
    void shutdownGracefully();

    /**
     * @see io.netty.util.concurrent.EventExecutorGroup#awaitTermination
     */
    void awaitTermination();

private:
    mutable asio::io_context io_context_;
    asio::executor_work_guard<Executor> work_guard_;
    std::jthread thread_;
};

/**
 * Hands out the loops that channels register to, in turn.
 *
 * @see io.netty.channel.EventLoopGroup
 * @see io.netty.channel.MultiThreadIoEventLoopGroup
 */
class EventLoopGroup {
public:
    explicit EventLoopGroup(std::size_t threads);

    /**
     * @see io.netty.channel.EventLoopGroup#next
     */
    [[nodiscard]] EventLoop &next();

    [[nodiscard]] std::size_t size() const { return loops_.size(); }
    [[nodiscard]] auto begin() { return loops_.begin(); }
    [[nodiscard]] auto end() { return loops_.end(); }

    void shutdownGracefully();
    void awaitTermination();

private:
    std::deque<EventLoop> loops_;
    std::atomic<std::size_t> next_{0};
};

}  // namespace endgate
