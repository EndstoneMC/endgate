#include <chrono>
#include <cstddef>

#include <gtest/gtest.h>

#include "endgate/network/rtc/sctp/cookie.h"

namespace endgate::rtc::sctp {
namespace {

using namespace std::chrono_literals;

constexpr StateCookie cookie{
    .local_tag = 0x11223344,
    .peer_tag = 0x55667788,
    .local_initial_tsn = 0xfffffff0,
    .peer_initial_tsn = 7,
    .peer_a_rwnd = 131072,
    .peer_outbound_streams = 1024,
    .peer_inbound_streams = 65535,
    .tie_tag = 0x0102030405060708,
    .peer_supports_forward_tsn = true,
    .peer_supports_reconfig = false,
};

constexpr StateCookie::Key key{std::byte{1}, std::byte{2}, std::byte{3}};
const auto issued = std::chrono::steady_clock::time_point{} + 1h;

TEST(StateCookie, RoundTrips)
{
    const auto sealed = cookie.seal(key, issued);
    const auto opened = StateCookie::open(key, sealed, issued + 1s, 60s);
    ASSERT_TRUE(opened);
    EXPECT_FALSE(opened->expired);
    EXPECT_EQ(opened->cookie.local_tag, cookie.local_tag);
    EXPECT_EQ(opened->cookie.peer_tag, cookie.peer_tag);
    EXPECT_EQ(opened->cookie.local_initial_tsn, cookie.local_initial_tsn);
    EXPECT_EQ(opened->cookie.peer_initial_tsn, cookie.peer_initial_tsn);
    EXPECT_EQ(opened->cookie.peer_a_rwnd, cookie.peer_a_rwnd);
    EXPECT_EQ(opened->cookie.peer_outbound_streams, cookie.peer_outbound_streams);
    EXPECT_EQ(opened->cookie.peer_inbound_streams, cookie.peer_inbound_streams);
    EXPECT_EQ(opened->cookie.tie_tag, cookie.tie_tag);
    EXPECT_TRUE(opened->cookie.peer_supports_forward_tsn);
    EXPECT_FALSE(opened->cookie.peer_supports_reconfig);
}

TEST(StateCookie, StaysSmall)
{
    EXPECT_EQ(cookie.seal(key, issued).size(), 73U);
}

TEST(StateCookie, RejectsAnyAlteredByte)
{
    const auto sealed = cookie.seal(key, issued);
    for (std::size_t i = 0; i < sealed.size(); ++i) {
        auto altered = sealed;
        altered[i] ^= std::byte{0x01};
        EXPECT_FALSE(StateCookie::open(key, altered, issued, 60s)) << i;
    }
}

TEST(StateCookie, RejectsAnotherKey)
{
    auto other = key;
    other[31] = std::byte{9};
    EXPECT_FALSE(StateCookie::open(other, cookie.seal(key, issued), issued, 60s));
}

TEST(StateCookie, RejectsWrongSize)
{
    auto sealed = cookie.seal(key, issued);
    sealed.push_back(std::byte{0});
    EXPECT_FALSE(StateCookie::open(key, sealed, issued, 60s));
    sealed.resize(10);
    EXPECT_FALSE(StateCookie::open(key, sealed, issued, 60s));
    EXPECT_FALSE(StateCookie::open(key, {}, issued, 60s));
}

TEST(StateCookie, ReportsExpiryWithoutRejecting)
{
    const auto sealed = cookie.seal(key, issued);
    const auto at_limit = StateCookie::open(key, sealed, issued + 60s, 60s);
    ASSERT_TRUE(at_limit);
    EXPECT_FALSE(at_limit->expired);
    const auto late = StateCookie::open(key, sealed, issued + 60s + 1ms, 60s);
    ASSERT_TRUE(late);
    EXPECT_TRUE(late->expired);
}

TEST(StateCookie, RejectsCookieFromTheFuture)
{
    EXPECT_FALSE(StateCookie::open(key, cookie.seal(key, issued + 1s), issued, 60s));
}

}  // namespace
}  // namespace endgate::rtc::sctp
