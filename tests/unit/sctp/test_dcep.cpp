#include <chrono>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "bytes.h"
#include "endgate/network/rtc/sctp/dcep.h"

namespace endgate::rtc {
namespace {

using namespace std::chrono_literals;
using sctp::bytes;

const auto pion_open =
    bytes({0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 'f', 'o', 'o', 'b', 'a', 'r'});

const auto unreliable_open =
    bytes({0x03, 0x81, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 'U', 'n', 'r', 'e', 'l',
           'i',  'a',  'b',  'l',  'e',  'D',  'a',  't',  'a',  'C',  'h',  'a',  'n', 'n', 'e', 'l'});

TEST(DataChannelOpen, ParsesPionGolden)
{
    const auto open = DataChannelOpen::parse(pion_open);
    ASSERT_TRUE(open);
    EXPECT_EQ(open->channel_type, 0x00);
    EXPECT_EQ(open->priority, 0);
    EXPECT_EQ(open->reliability_parameter, 0U);
    EXPECT_EQ(open->label, "foo");
    EXPECT_EQ(open->protocol, "bar");
    EXPECT_EQ(open->reliability(), (Reliability{}));
    EXPECT_EQ(open->serialize(), pion_open);
}

TEST(DataChannelOpen, ReadsNetherNetUnreliableChannel)
{
    const auto open = DataChannelOpen::parse(unreliable_open);
    ASSERT_TRUE(open);
    EXPECT_EQ(open->label, "UnreliableDataChannel");
    EXPECT_TRUE(open->protocol.empty());
    EXPECT_EQ(open->priority, 256);
    EXPECT_EQ(open->reliability(), (Reliability{.unordered = true, .max_retransmits = 0, .max_packet_life_time = {}}));
}

TEST(DataChannelOpen, WritesEveryChannelType)
{
    EXPECT_EQ(DataChannelOpen::of("UnreliableDataChannel", "",
                                  {.unordered = true, .max_retransmits = 0, .max_packet_life_time = {}}, 256)
                  .serialize(),
              unreliable_open);
    EXPECT_EQ(DataChannelOpen::of("a", "", {}, 256).channel_type, 0x00);
    EXPECT_EQ(DataChannelOpen::of("a", "", {.unordered = true}, 256).channel_type, 0x80);
    EXPECT_EQ(DataChannelOpen::of("a", "", {.max_retransmits = 3}, 256).channel_type, 0x01);
    EXPECT_EQ(DataChannelOpen::of("a", "", {.unordered = true, .max_retransmits = 3}, 256).channel_type, 0x81);
    const auto timed = DataChannelOpen::of("a", "", {.max_packet_life_time = 1500ms}, 256);
    EXPECT_EQ(timed.channel_type, 0x02);
    EXPECT_EQ(timed.reliability_parameter, 1500U);
    EXPECT_EQ(DataChannelOpen::of("a", "", {.unordered = true, .max_packet_life_time = 10ms}, 256).channel_type, 0x82);
}

TEST(DataChannelOpen, RoundTripsReliability)
{
    for (const auto &reliability : {Reliability{}, Reliability{.unordered = true}, Reliability{.max_retransmits = 7},
                                    Reliability{.unordered = true, .max_packet_life_time = 250ms}}) {
        const auto open = DataChannelOpen::parse(DataChannelOpen::of("label", "proto", reliability, 512).serialize());
        ASSERT_TRUE(open);
        EXPECT_EQ(open->reliability(), reliability);
        EXPECT_EQ(open->priority, 512);
        EXPECT_EQ(open->protocol, "proto");
    }
}

TEST(DataChannelOpen, RejectsMalformedMessages)
{
    EXPECT_FALSE(DataChannelOpen::parse({}));
    EXPECT_FALSE(DataChannelOpen::parse(std::span{pion_open}.first(11)));
    EXPECT_FALSE(DataChannelOpen::parse(std::span{pion_open}.first(17)));
    auto longer = pion_open;
    longer.push_back(std::byte{0});
    EXPECT_FALSE(DataChannelOpen::parse(longer));
    auto ack = pion_open;
    ack[0] = std::byte{0x02};
    EXPECT_FALSE(DataChannelOpen::parse(ack));
    auto overstated = pion_open;
    overstated[9] = std::byte{0xff};
    EXPECT_FALSE(DataChannelOpen::parse(overstated));
}

TEST(DataChannelOpen, RefusesUnknownChannelType)
{
    auto unknown = pion_open;
    unknown[1] = std::byte{0x03};
    const auto open = DataChannelOpen::parse(unknown);
    ASSERT_TRUE(open);
    EXPECT_FALSE(open->reliability());
    unknown[1] = std::byte{0x40};
    EXPECT_FALSE(DataChannelOpen::parse(unknown)->reliability());
}

}  // namespace
}  // namespace endgate::rtc
