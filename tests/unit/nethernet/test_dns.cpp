#include <chrono>
#include <vector>

#include <asio/ip/address.hpp>
#include <gtest/gtest.h>

#include "endgate/network/nethernet/dns.h"

namespace {

using endgate::nethernet::DnsCache;
using std::chrono::minutes;
using std::chrono::seconds;

const auto NOW = DnsCache::Clock::time_point{} + std::chrono::hours{1};

std::vector<asio::ip::address> addresses(const char *address)
{
    return {asio::ip::make_address(address)};
}

TEST(DnsCacheTest, AnswersFromTheCacheUntilTheEntryExpires)
{
    DnsCache cache;
    cache.add("play.example.com", addresses("203.0.113.7"), NOW);

    EXPECT_EQ(cache.tryGet("play.example.com", NOW + minutes{4}), addresses("203.0.113.7"));
    EXPECT_EQ(cache.tryGet("play.example.com", NOW + minutes{5}), std::nullopt);
    EXPECT_EQ(cache.size(), 0U);
}

TEST(DnsCacheTest, MissesAHostnameItWasNeverGiven)
{
    DnsCache cache;
    cache.add("play.example.com", addresses("203.0.113.7"), NOW);

    EXPECT_EQ(cache.tryGet("lobby.example.com", NOW), std::nullopt);
}

TEST(DnsCacheTest, AddingAgainReplacesTheAnswerAndRenewsIt)
{
    DnsCache cache;
    cache.add("play.example.com", addresses("203.0.113.7"), NOW);
    cache.add("play.example.com", addresses("2001:db8::7"), NOW + minutes{4});

    EXPECT_EQ(cache.tryGet("play.example.com", NOW + minutes{8}), addresses("2001:db8::7"));
    EXPECT_EQ(cache.size(), 1U);
}

TEST(DnsCacheTest, DropsTheAnswerClosestToExpiringWhenFull)
{
    DnsCache cache({.lifetime = minutes{5}, .capacity = 2});
    cache.add("a.example.com", addresses("203.0.113.1"), NOW);
    cache.add("b.example.com", addresses("203.0.113.2"), NOW + seconds{1});
    cache.add("c.example.com", addresses("203.0.113.3"), NOW + seconds{2});

    EXPECT_EQ(cache.size(), 2U);
    EXPECT_EQ(cache.tryGet("a.example.com", NOW + seconds{2}), std::nullopt);
    EXPECT_EQ(cache.tryGet("b.example.com", NOW + seconds{2}), addresses("203.0.113.2"));
    EXPECT_EQ(cache.tryGet("c.example.com", NOW + seconds{2}), addresses("203.0.113.3"));
}

TEST(DnsCacheTest, ExpiredAnswersMakeRoomBeforeLiveOnes)
{
    DnsCache cache({.lifetime = minutes{5}, .capacity = 2});
    cache.add("a.example.com", addresses("203.0.113.1"), NOW);
    cache.add("b.example.com", addresses("203.0.113.2"), NOW + minutes{4});
    cache.add("c.example.com", addresses("203.0.113.3"), NOW + minutes{6});

    EXPECT_EQ(cache.tryGet("b.example.com", NOW + minutes{6}), addresses("203.0.113.2"));
    EXPECT_EQ(cache.tryGet("c.example.com", NOW + minutes{6}), addresses("203.0.113.3"));
}

}  // namespace
