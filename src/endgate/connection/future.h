#pragma once

#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace endgate {

class MinecraftConnection;

/**
 * The result of an I/O operation on a connection. Its listeners run once it completes, on the
 * connection's event loop.
 *
 * @see io.netty.channel.ChannelFuture
 */
class ChannelFuture {
public:
    /**
     * @see io.netty.channel.ChannelFuture#channel
     */
    [[nodiscard]] MinecraftConnection &channel() const { return *state().channel; }

    /**
     * @see io.netty.util.concurrent.Future#isDone
     */
    [[nodiscard]] bool isDone() const { return state_ && state_->done; }

    /**
     * @see io.netty.util.concurrent.Future#isSuccess
     */
    [[nodiscard]] bool isSuccess() const { return isDone() && !state_->cause; }

    /**
     * @see io.netty.util.concurrent.Future#cause
     */
    [[nodiscard]] std::exception_ptr cause() const { return state_ ? state_->cause : nullptr; }

    /**
     * @see io.netty.channel.ChannelFuture#isVoid
     */
    [[nodiscard]] bool isVoid() const { return !state_; }

    /**
     * Runs the listener once the future completes, or now if it already has.
     *
     * @see io.netty.channel.ChannelFuture#addListener
     */
    template <std::invocable<const ChannelFuture &> Listener>
    ChannelFuture &addListener(Listener listener)
    {
        if (isDone()) {
            listener(*this);
            return *this;
        }
        state().listeners.emplace_back(std::move(listener));
        return *this;
    }

protected:
    struct State {
        std::shared_ptr<MinecraftConnection> channel;
        std::vector<std::function<void(const ChannelFuture &)>> listeners;
        std::exception_ptr cause;
        bool done = false;
    };

    ChannelFuture() = default;
    explicit ChannelFuture(std::shared_ptr<State> state) : state_(std::move(state)) {}

    [[nodiscard]] State &state() const
    {
        if (!state_) {
            throw std::logic_error("void future");
        }
        return *state_;
    }

    std::shared_ptr<State> state_;
};

/**
 * A ChannelFuture the transport completes. A default-constructed one is the void promise: completing
 * it does nothing.
 *
 * @see io.netty.channel.ChannelPromise
 * @see io.netty.channel.VoidChannelPromise
 */
class ChannelPromise : public ChannelFuture {
public:
    ChannelPromise() = default;
    explicit ChannelPromise(std::shared_ptr<MinecraftConnection> channel)
        : ChannelFuture(std::make_shared<State>(State{.channel = std::move(channel)}))
    {
    }

    /**
     * @see io.netty.util.concurrent.Promise#trySuccess
     */
    bool trySuccess() { return complete(nullptr); }

    /**
     * @see io.netty.util.concurrent.Promise#tryFailure
     */
    bool tryFailure(std::exception_ptr cause) { return complete(std::move(cause)); }

private:
    bool complete(std::exception_ptr cause)
    {
        if (!state_ || state_->done) {
            return false;
        }
        state_->done = true;
        state_->cause = std::move(cause);
        for (const auto &listener : std::exchange(state_->listeners, {})) {
            listener(*this);
        }
        return true;
    }
};

/**
 * @see io.netty.channel.ChannelFutureListener
 */
class ChannelFutureListener {
public:
    /**
     * Closes the connection the future belongs to, whether the operation succeeded or not.
     *
     * @see io.netty.channel.ChannelFutureListener#CLOSE
     */
    static constexpr auto CLOSE = [](const auto &future) {
        future.channel().close();
    };
};

}  // namespace endgate
