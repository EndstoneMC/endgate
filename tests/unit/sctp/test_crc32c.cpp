#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "endgate/network/rtc/sctp/crc32c.h"

namespace endgate::rtc::sctp {
namespace {

std::vector<std::byte> bytes(std::initializer_list<std::uint8_t> values)
{
    std::vector<std::byte> result;
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

std::vector<std::byte> filled(std::size_t size, std::uint8_t start, int step)
{
    std::vector<std::byte> result(size);
    for (std::size_t i = 0; i < size; ++i) {
        result[i] = static_cast<std::byte>(start + step * static_cast<int>(i));
    }
    return result;
}

TEST(Crc32c, CheckValue)
{
    constexpr std::string_view input = "123456789";
    EXPECT_EQ(crc32c(std::as_bytes(std::span{input})), 0xE3069283U);
}

TEST(Crc32c, Rfc3720Vectors)
{
    EXPECT_EQ(crc32c(filled(32, 0x00, 0)), 0x8A9136AAU);
    EXPECT_EQ(crc32c(filled(32, 0xFF, 0)), 0x62A8AB43U);
    EXPECT_EQ(crc32c(filled(32, 0, 1)), 0x46DD794EU);
    EXPECT_EQ(crc32c(filled(32, 31, -1)), 0x113FDB5CU);
    const auto iscsi_read_command =
        bytes({0x01, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18,
               0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    EXPECT_EQ(crc32c(iscsi_read_command), 0xD9963A56U);
}

TEST(Crc32c, ShortInputs)
{
    EXPECT_EQ(crc32c({}), 0U);
    EXPECT_EQ(crc32c(bytes({0x00})), 0x527D5351U);
    EXPECT_EQ(crc32c(bytes({0x00, 0x00, 0x00, 0x00})), 0x48674BC7U);
    EXPECT_EQ(crc32c(bytes({1, 2, 3, 4})), 0x29308CF4U);
    EXPECT_EQ(crc32c(bytes({1, 2, 3, 4, 5, 6, 7, 8})), 0x46891F81U);
}

TEST(Crc32c, EveryLengthMatchesBytewise)
{
    const auto data = filled(67, 7, 13);
    for (std::size_t length = 0; length <= data.size(); ++length) {
        std::uint32_t expected = 0xFFFFFFFFU;
        for (std::size_t i = 0; i < length; ++i) {
            expected ^= std::to_integer<std::uint32_t>(data[i]);
            for (auto bit = 0; bit < 8; ++bit) {
                expected = (expected & 1U) ? (expected >> 1U) ^ 0x82F63B78U : expected >> 1U;
            }
        }
        EXPECT_EQ(crc32c(std::span{data}.first(length)), ~expected) << length;
    }
}

TEST(Crc32c, ContinuesFromPreviousValue)
{
    const auto data = filled(41, 3, 5);
    for (std::size_t split = 0; split <= data.size(); ++split) {
        const auto head = crc32c(std::span{data}.first(split));
        EXPECT_EQ(crc32c(std::span{data}.subspan(split), head), crc32c(data)) << split;
    }
}

TEST(Crc32c, MatchesTheBitwiseDefinitionAtEveryLengthAndAlignment)
{
    std::seed_seq seed{1};
    std::mt19937 random{seed};
    std::vector<std::byte> data(1024 + 8);
    for (auto &byte : data) {
        byte = static_cast<std::byte>(random());
    }
    for (std::size_t offset = 0; offset < 8; ++offset) {
        for (std::size_t length = 0; length <= 1024; ++length) {
            const auto input = std::span{data}.subspan(offset, length);
            std::uint32_t expected = 0xFFFFFFFFU;
            for (const auto byte : input) {
                expected ^= std::to_integer<std::uint32_t>(byte);
                for (auto bit = 0; bit < 8; ++bit) {
                    expected = (expected & 1U) ? (expected >> 1U) ^ 0x82F63B78U : expected >> 1U;
                }
            }
            ASSERT_EQ(crc32c(input), ~expected) << "offset " << offset << " length " << length;
        }
    }
}

TEST(Crc32c, ChainsAcrossCalls)
{
    const auto data = filled(1000, 7, 13);
    const auto whole = crc32c(data);
    for (const std::size_t split : {0, 1, 7, 8, 9, 500, 999, 1000}) {
        const auto head = std::span{data}.first(split);
        const auto tail = std::span{data}.subspan(split);
        EXPECT_EQ(crc32c(tail, crc32c(head)), whole) << "split " << split;
    }
}

}  // namespace
}  // namespace endgate::rtc::sctp
