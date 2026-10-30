#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <optional>
#include <thread>

#include <gtest/gtest.h>

#include "endgate/network/eventloop.h"
#include "endgate/network/pipeline.h"
#include "endgate/network/timeout.h"

namespace endgate {
namespace {

using std::chrono::milliseconds;

struct TimeoutSink {
    std::promise<void> closed;
    std::atomic<bool> timed_out{false};
    std::atomic<bool> is_closed{false};

    void channelActive() {}
    void channelInactive() {}
    void channelRead(int) {}
    void channelReadComplete() {}
    void exceptionCaught(const std::exception &cause)
    {
        timed_out = dynamic_cast<const ReadTimeoutException *>(&cause) != nullptr;
    }
    void write(int) {}
    void flush() {}
    void close()
    {
        is_closed = true;
        closed.set_value();
    }
};

class ReadTimeoutHandlerTest : public ::testing::Test {
protected:
    static constexpr milliseconds TIMEOUT{100};

    void start(milliseconds timeout)
    {
        pipeline_.emplace(sink_, ReadTimeoutHandler{loop_, timeout});
        loop_.execute([this] { pipeline_->fireChannelActive(); });
    }

    void read()
    {
        loop_.execute([this] {
            pipeline_->fireChannelRead(1);
            pipeline_->fireChannelReadComplete();
        });
    }

    void stop()
    {
        std::promise<void> done;
        loop_.execute([&] {
            pipeline_->fireChannelInactive();
            done.set_value();
        });
        done.get_future().wait();
    }

    EventLoop loop_;
    TimeoutSink sink_;
    std::optional<ChannelPipeline<TimeoutSink, ReadTimeoutHandler>> pipeline_;
};

}  // namespace

TEST_F(ReadTimeoutHandlerTest, RaisesReadTimeoutExceptionAndClosesAfterTheTimeoutOfSilence)
{
    const auto started = std::chrono::steady_clock::now();
    start(TIMEOUT);

    ASSERT_EQ(sink_.closed.get_future().wait_for(std::chrono::seconds{5}), std::future_status::ready);
    EXPECT_GE(std::chrono::steady_clock::now() - started, TIMEOUT);
    EXPECT_TRUE(sink_.timed_out);
    stop();
}

TEST_F(ReadTimeoutHandlerTest, EachCompletedReadRestartsTheTimeout)
{
    const auto started = std::chrono::steady_clock::now();
    start(TIMEOUT);
    for (int i = 0; i < 8; ++i) {
        std::this_thread::sleep_for(milliseconds{40});
        read();
    }
    const auto last_read = std::chrono::steady_clock::now();
    EXPECT_FALSE(sink_.is_closed);

    ASSERT_EQ(sink_.closed.get_future().wait_for(std::chrono::seconds{5}), std::future_status::ready);
    EXPECT_GE(std::chrono::steady_clock::now() - started, TIMEOUT + milliseconds{300});
    EXPECT_GE(std::chrono::steady_clock::now() - last_read, TIMEOUT - milliseconds{40});
    stop();
}

TEST_F(ReadTimeoutHandlerTest, DoesNotTimeOutWhileAReadIsInProgress)
{
    start(TIMEOUT);
    loop_.execute([this] { pipeline_->fireChannelRead(1); });
    std::this_thread::sleep_for(TIMEOUT * 3);
    EXPECT_FALSE(sink_.is_closed);
    stop();
}

TEST_F(ReadTimeoutHandlerTest, ZeroTimeoutTurnsItOff)
{
    start(milliseconds{0});
    std::this_thread::sleep_for(TIMEOUT);
    EXPECT_FALSE(sink_.is_closed);
    stop();
}

TEST_F(ReadTimeoutHandlerTest, StopsWatchingOnceTheChannelIsInactive)
{
    start(TIMEOUT);
    stop();
    std::this_thread::sleep_for(TIMEOUT * 3);
    EXPECT_FALSE(sink_.is_closed);
}

}  // namespace endgate
