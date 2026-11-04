#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "endgate/connection/future.h"
#include "support.h"

namespace endgate {
namespace {

class ChannelFutureTest : public testing::Test {
protected:
    ProxyServer server_;
    Registry registry_;
    Node node_{server_, registry_};
    std::shared_ptr<TestConnection> connection_ =
        std::make_shared<TestConnection>(node_.endpoint(), "127.0.0.1:1", Direction::Serverbound);
};

TEST_F(ChannelFutureTest, CloseWithClosesOnlyOnceTheDisconnectIsWritten)
{
    connection_->holdWrites();
    node_.call([&] {
        connection_->closeWith(DisconnectFailReason::NoReason, "You are already connected to this proxy!");
        return 0;
    });
    EXPECT_EQ(connection_->sent(), 1U);
    EXPECT_FALSE(connection_->closed());

    node_.call([&] {
        connection_->completeWrites();
        return 0;
    });
    EXPECT_TRUE(connection_->closed());
}

TEST_F(ChannelFutureTest, AListenerRunsOnceWhenThePromiseCompletes)
{
    auto promise = connection_->newPromise();
    int runs = 0;
    promise.addListener([&](const ChannelFuture &) { ++runs; });
    EXPECT_FALSE(promise.isDone());

    EXPECT_TRUE(promise.trySuccess());
    EXPECT_FALSE(promise.trySuccess());
    EXPECT_FALSE(promise.tryFailure(std::make_exception_ptr(std::runtime_error("late"))));
    EXPECT_EQ(runs, 1);
    EXPECT_TRUE(promise.isSuccess());
}

TEST_F(ChannelFutureTest, AListenerAddedToACompletedFutureRunsAtOnce)
{
    auto promise = connection_->newPromise();
    promise.trySuccess();
    bool ran = false;
    promise.addListener([&](const ChannelFuture &future) { ran = &future.channel() == connection_.get(); });
    EXPECT_TRUE(ran);
}

TEST_F(ChannelFutureTest, AFailedPromiseIsDoneWithoutSuccess)
{
    auto promise = connection_->newPromise();
    EXPECT_TRUE(promise.tryFailure(std::make_exception_ptr(std::runtime_error("closed"))));
    EXPECT_TRUE(promise.isDone());
    EXPECT_FALSE(promise.isSuccess());
    EXPECT_NE(promise.cause(), nullptr);
}

TEST_F(ChannelFutureTest, TheVoidPromiseTakesNoListener)
{
    auto promise = MinecraftConnection::voidPromise();
    EXPECT_TRUE(promise.isVoid());
    EXPECT_FALSE(promise.trySuccess());
    EXPECT_THROW(promise.addListener([](const ChannelFuture &) {}), std::logic_error);
}

}  // namespace
}  // namespace endgate
