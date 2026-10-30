#include <chrono>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "endgate/util/ratelimit.h"

namespace endgate {

using std::chrono::milliseconds;

TEST(CaffeineCacheRatelimiter, AllowsTheFirstAttemptAndRefusesTheNextWithinTheTimeout)
{
    CaffeineCacheRatelimiter<std::string>::Clock::time_point now{};
    CaffeineCacheRatelimiter<std::string> limiter(milliseconds{3000}, [&now] { return now; });

    EXPECT_TRUE(limiter.attempt("a"));
    now += milliseconds{1};
    EXPECT_FALSE(limiter.attempt("a"));
    now += milliseconds{2997};
    EXPECT_FALSE(limiter.attempt("a"));
}

TEST(CaffeineCacheRatelimiter, AllowsAgainOnceTheTimeoutHasPassed)
{
    CaffeineCacheRatelimiter<std::string>::Clock::time_point now{};
    CaffeineCacheRatelimiter<std::string> limiter(milliseconds{3000}, [&now] { return now; });

    EXPECT_TRUE(limiter.attempt("a"));
    now += milliseconds{3000};
    EXPECT_TRUE(limiter.attempt("a"));
}

TEST(CaffeineCacheRatelimiter, RefusedAttemptsDoNotExtendTheTimeout)
{
    CaffeineCacheRatelimiter<std::string>::Clock::time_point now{};
    CaffeineCacheRatelimiter<std::string> limiter(milliseconds{3000}, [&now] { return now; });

    EXPECT_TRUE(limiter.attempt("a"));
    now += milliseconds{2000};
    EXPECT_FALSE(limiter.attempt("a"));
    now += milliseconds{1000};
    EXPECT_TRUE(limiter.attempt("a"));
}

TEST(CaffeineCacheRatelimiter, LimitsEachKeyOnItsOwn)
{
    CaffeineCacheRatelimiter<std::string>::Clock::time_point now{};
    CaffeineCacheRatelimiter<std::string> limiter(milliseconds{3000}, [&now] { return now; });

    EXPECT_TRUE(limiter.attempt("a"));
    now += milliseconds{1000};
    EXPECT_TRUE(limiter.attempt("b"));
    EXPECT_FALSE(limiter.attempt("a"));
    EXPECT_FALSE(limiter.attempt("b"));
    now += milliseconds{2000};
    EXPECT_TRUE(limiter.attempt("a"));
    EXPECT_FALSE(limiter.attempt("b"));
    now += milliseconds{1000};
    EXPECT_TRUE(limiter.attempt("b"));
}

TEST(Ratelimiters, ZeroMillisecondsAllowsEverything)
{
    const auto limiter = Ratelimiters::createWithMilliseconds<std::string>(0);
    EXPECT_TRUE(limiter->attempt("a"));
    EXPECT_TRUE(limiter->attempt("a"));
}

TEST(Ratelimiters, PositiveMillisecondsLimitsRepeatAttempts)
{
    const auto limiter = Ratelimiters::createWithMilliseconds<std::string>(60000);
    EXPECT_TRUE(limiter->attempt("a"));
    EXPECT_FALSE(limiter->attempt("a"));
}

}  // namespace endgate
