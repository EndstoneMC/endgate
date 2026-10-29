#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <asio/steady_timer.hpp>
#include <gtest/gtest.h>

#include "endgate/network/eventloop.h"

namespace endgate {

TEST(EventLoop, IsNotInTheEventLoopFromOutside)
{
    EventLoop loop;
    EXPECT_FALSE(loop.inEventLoop());
}

TEST(EventLoop, ExecuteRunsTheTaskOnTheLoopThread)
{
    EventLoop loop;
    std::promise<std::pair<bool, std::thread::id>> ran;
    loop.execute([&] { ran.set_value({loop.inEventLoop(), std::this_thread::get_id()}); });
    const auto [inside, id] = ran.get_future().get();
    EXPECT_TRUE(inside);
    EXPECT_NE(id, std::this_thread::get_id());
}

TEST(EventLoop, ExecuteRunsTasksInOrder)
{
    EventLoop loop;
    std::vector<int> order;
    std::promise<void> done;
    for (int i = 0; i < 100; ++i) {
        loop.execute([&order, i] { order.push_back(i); });
    }
    loop.execute([&done] { done.set_value(); });
    done.get_future().wait();
    ASSERT_EQ(order.size(), 100U);
    EXPECT_TRUE(std::ranges::is_sorted(order));
}

TEST(EventLoop, ExecuteFromTheLoopQueuesBehindTheCurrentTask)
{
    EventLoop loop;
    std::vector<int> order;
    std::promise<void> done;
    loop.execute([&] {
        loop.execute([&] {
            order.push_back(2);
            done.set_value();
        });
        order.push_back(1);
    });
    done.get_future().wait();
    EXPECT_EQ(order, (std::vector{1, 2}));
}

TEST(EventLoop, TimersFireOnTheLoopThread)
{
    EventLoop loop;
    asio::steady_timer timer(loop.getExecutor(), std::chrono::milliseconds{10});
    std::promise<bool> fired;
    timer.async_wait([&](const asio::error_code &) { fired.set_value(loop.inEventLoop()); });
    EXPECT_TRUE(fired.get_future().get());
}

TEST(EventLoop, AwaitTerminationJoinsAfterShutdown)
{
    EventLoop loop;
    std::atomic<bool> ran{false};
    loop.execute([&] { ran = true; });
    loop.shutdownGracefully();
    loop.awaitTermination();
    EXPECT_TRUE(ran);
}

TEST(EventLoop, ScheduleRunsTheTaskOnTheLoopAfterTheDelay)
{
    EventLoop loop;
    std::promise<bool> ran;
    const auto started = std::chrono::steady_clock::now();
    const auto future = loop.schedule([&] { ran.set_value(loop.inEventLoop()); }, std::chrono::milliseconds{50});
    EXPECT_TRUE(ran.get_future().get());
    EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds{50});
}

TEST(EventLoop, CancelledScheduleNeverRuns)
{
    EventLoop loop;
    std::atomic<bool> ran{false};
    const auto future = loop.schedule([&] { ran = true; }, std::chrono::milliseconds{100});
    future->cancel();
    std::promise<void> later;
    loop.schedule([&] { later.set_value(); }, std::chrono::milliseconds{300});
    later.get_future().wait();
    EXPECT_FALSE(ran);
}

TEST(EventLoop, DestroyedFromItsOwnThreadDoesNotJoinItself)
{
    std::promise<void> destroyed;
    auto loop = std::make_unique<EventLoop>();
    auto *const raw = loop.get();
    raw->execute([&destroyed, owned = std::move(loop)]() mutable {
        owned.reset();
        destroyed.set_value();
    });
    destroyed.get_future().wait();
}

TEST(EventLoopGroup, NextCyclesThroughEveryLoop)
{
    EventLoopGroup group(3);
    ASSERT_EQ(group.size(), 3U);
    std::set<EventLoop *> seen;
    for (int i = 0; i < 3; ++i) {
        seen.insert(&group.next());
    }
    EXPECT_EQ(seen.size(), 3U);
}

TEST(EventLoopGroup, NextWrapsAround)
{
    EventLoopGroup group(2);
    auto &first = group.next();
    auto &second = group.next();
    EXPECT_NE(&first, &second);
    EXPECT_EQ(&group.next(), &first);
    EXPECT_EQ(&group.next(), &second);
}

TEST(EventLoopGroup, EveryLoopRunsOnItsOwnThread)
{
    EventLoopGroup group(4);
    std::set<std::thread::id> threads;
    std::mutex mutex;
    std::latch done(4);
    for (auto &loop : group) {
        loop.execute([&] {
            const std::scoped_lock lock(mutex);
            threads.insert(std::this_thread::get_id());
            done.count_down();
        });
    }
    done.wait();
    EXPECT_EQ(threads.size(), 4U);
}

TEST(EventLoopGroup, ShutdownDrainsQueuedTasks)
{
    EventLoopGroup group(2);
    std::atomic<int> ran{0};
    for (auto &loop : group) {
        loop.execute([&] { ++ran; });
    }
    group.shutdownGracefully();
    group.awaitTermination();
    EXPECT_EQ(ran, 2);
}

}  // namespace endgate
