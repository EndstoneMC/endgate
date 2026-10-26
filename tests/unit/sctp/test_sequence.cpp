#include <cstdint>

#include <gtest/gtest.h>

#include "endgate/network/rtc/sctp/sequence.h"

namespace endgate::rtc::sctp {
namespace {

constexpr std::uint64_t tsn_base = std::uint64_t{1} << 32U;

TEST(Unwrap, FollowsTsnAcrossWrap)
{
    const auto reference = tsn_base + 0xFFFFFFF0U;
    EXPECT_EQ(unwrap<std::uint32_t>(0xFFFFFFF0U, reference), reference);
    EXPECT_EQ(unwrap<std::uint32_t>(0xFFFFFFFFU, reference), reference + 15);
    EXPECT_EQ(unwrap<std::uint32_t>(0U, reference), reference + 16);
    EXPECT_EQ(unwrap<std::uint32_t>(5U, reference), reference + 21);
    EXPECT_EQ(unwrap<std::uint32_t>(0xFFFFFF00U, reference), reference - 0xF0);
}

TEST(Unwrap, GoesBackwardsAcrossWrap)
{
    const auto reference = 2 * tsn_base + 3;
    EXPECT_EQ(unwrap<std::uint32_t>(0xFFFFFFFEU, reference), reference - 5);
}

TEST(Unwrap, SplitsAtHalfTheSpace)
{
    const auto reference = tsn_base;
    EXPECT_EQ(unwrap<std::uint32_t>(0x7FFFFFFFU, reference), reference + 0x7FFFFFFF);
    EXPECT_EQ(unwrap<std::uint32_t>(0x80000000U, reference), reference - 0x80000000);
}

TEST(Unwrap, FollowsSsnAcrossWrap)
{
    const std::uint64_t reference = 0x10000 + 0xFFFE;
    EXPECT_EQ(unwrap<std::uint16_t>(0xFFFF, reference), reference + 1);
    EXPECT_EQ(unwrap<std::uint16_t>(0, reference), reference + 2);
    EXPECT_EQ(unwrap<std::uint16_t>(0xFFF0, reference), reference - 14);
}

}  // namespace
}  // namespace endgate::rtc::sctp
