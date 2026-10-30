#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace endgate {

/**
 * Decides whether a key may act again, or has acted too recently.
 *
 * @see com.velocitypowered.proxy.util.ratelimit.Ratelimiter
 */
template <typename T>
class Ratelimiter {
public:
    Ratelimiter() = default;
    Ratelimiter(const Ratelimiter &) = delete;
    Ratelimiter &operator=(const Ratelimiter &) = delete;
    Ratelimiter(Ratelimiter &&) = delete;
    Ratelimiter &operator=(Ratelimiter &&) = delete;
    virtual ~Ratelimiter() = default;

    /**
     * Attempts to rate-limit the object.
     *
     * @return true if we should allow the object, false if we should rate-limit
     * @see com.velocitypowered.proxy.util.ratelimit.Ratelimiter#attempt
     */
    [[nodiscard]] virtual bool attempt(const T &key) = 0;
};

/**
 * Allows a key once per timeout: an allowed key is remembered for the timeout, and attempts in that
 * time are refused without extending it. Safe to call from any thread.
 *
 * @see com.velocitypowered.proxy.util.ratelimit.CaffeineCacheRatelimiter
 */
template <typename T>
class CaffeineCacheRatelimiter final : public Ratelimiter<T> {
public:
    using Clock = std::chrono::steady_clock;
    using Ticker = std::function<Clock::time_point()>;

    explicit CaffeineCacheRatelimiter(Clock::duration timeout, Ticker ticker = &Clock::now)
        : timeout_(timeout), ticker_(std::move(ticker))
    {
    }

    [[nodiscard]] bool attempt(const T &key) override
    {
        const std::scoped_lock lock(mutex_);
        const auto now = ticker_();
        while (!written_.empty() && written_.front().second <= now) {
            keys_.erase(written_.front().first);
            written_.pop_front();
        }
        if (!keys_.insert(key).second) {
            return false;
        }
        written_.emplace_back(key, now + timeout_);
        return true;
    }

private:
    Clock::duration timeout_;
    Ticker ticker_;
    std::mutex mutex_;
    std::unordered_set<T> keys_;
    std::deque<std::pair<T, Clock::time_point>> written_;
};

/**
 * Allows everything.
 *
 * @see com.velocitypowered.proxy.util.ratelimit.NoopCacheRatelimiter
 */
template <typename T>
class NoopCacheRatelimiter final : public Ratelimiter<T> {
public:
    [[nodiscard]] bool attempt(const T &) override { return true; }
};

/**
 * @see com.velocitypowered.proxy.util.ratelimit.Ratelimiters
 */
class Ratelimiters {
public:
    Ratelimiters() = delete;

    /**
     * @see com.velocitypowered.proxy.util.ratelimit.Ratelimiters#createWithMilliseconds
     */
    template <typename T>
    [[nodiscard]] static std::unique_ptr<Ratelimiter<T>> createWithMilliseconds(std::int64_t ms)
    {
        if (ms <= 0) {
            return std::make_unique<NoopCacheRatelimiter<T>>();
        }
        return std::make_unique<CaffeineCacheRatelimiter<T>>(std::chrono::milliseconds{ms});
    }
};

}  // namespace endgate
