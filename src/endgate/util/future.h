#pragma once

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include <asio/any_io_executor.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/use_awaitable.hpp>

#include "endgate/awaitable.h"

namespace endgate {

/**
 * A value one side completes and one coroutine waits for, on one event loop.
 *
 * @see java.util.concurrent.CompletableFuture
 */
template <typename T>
class CompletableFuture {
public:
    explicit CompletableFuture(const asio::any_io_executor &executor) : channel_(executor, 1) {}

    /**
     * @see java.util.concurrent.CompletableFuture#complete
     */
    bool complete(T value)
    {
        if (std::exchange(done_, true)) {
            return false;
        }
        return channel_.try_send(std::exception_ptr{}, std::optional<T>{std::move(value)});
    }

    /**
     * @see java.util.concurrent.CompletableFuture#completeExceptionally
     */
    bool completeExceptionally(std::exception_ptr cause)
    {
        if (std::exchange(done_, true)) {
            return false;
        }
        return channel_.try_send(std::move(cause), std::optional<T>{});
    }

    /**
     * @see java.util.concurrent.CompletableFuture#isDone
     */
    [[nodiscard]] bool isDone() const { return done_; }

    /**
     * @see java.util.concurrent.CompletableFuture#get
     */
    Awaitable<T> get()
    {
        auto value = co_await channel_.async_receive(asio::use_awaitable);
        if (!value) {
            throw std::logic_error("completed with neither a value nor a cause");
        }
        co_return std::move(*value);
    }

private:
    asio::experimental::channel<void(std::exception_ptr, std::optional<T>)> channel_;
    bool done_{false};
};

}  // namespace endgate
