#include <optional>
#include <string>
#include <vector>

#include <asio/ip/address.hpp>
#include <gtest/gtest.h>

#include "endgate/network/nethernet/candidates.h"

namespace endgate::nethernet {
namespace {

std::vector<asio::ip::address> addressesOf(std::initializer_list<const char *> texts)
{
    std::vector<asio::ip::address> addresses;
    for (const auto *text : texts) {
        addresses.push_back(asio::ip::make_address(text));
    }
    return addresses;
}

std::vector<std::string> describe(const std::vector<rtc::CandidateAddress> &candidates)
{
    std::vector<std::string> described;
    for (const auto &[address, base] : candidates) {
        auto text = address.address().to_string() + ":" + std::to_string(address.port());
        if (base) {
            text += " srflx " + base->address().to_string() + ":" + std::to_string(base->port());
        }
        described.push_back(text);
    }
    return described;
}

TEST(GatherCandidatesTest, OffersTheReachedAddressAsHostWhenItIsLocal)
{
    const auto candidates = gatherCandidates(addressesOf({"212.227.14.80"}), 19134,
                                             addressesOf({"127.0.0.1", "212.227.14.80", "172.17.0.1"}), 19134);
    EXPECT_EQ(describe(candidates), (std::vector<std::string>{"212.227.14.80:19134", "172.17.0.1:19134"}));
}

TEST(GatherCandidatesTest, OffersTheReachedAddressAsServerReflexiveBehindNat)
{
    const auto candidates =
        gatherCandidates(addressesOf({"203.0.113.9"}), 19134, addressesOf({"10.0.0.5", "fe80::1", "127.0.0.1"}), 19132);
    EXPECT_EQ(describe(candidates),
              (std::vector<std::string>{"203.0.113.9:19134 srflx 10.0.0.5:19132", "10.0.0.5:19132"}));
}

TEST(GatherCandidatesTest, BasesEachFamilyOnAnAddressOfItsOwn)
{
    const auto candidates =
        gatherCandidates(addressesOf({"203.0.113.9", "2001:db8::9"}), 19134, addressesOf({"10.0.0.5"}), 19132);
    EXPECT_EQ(describe(candidates), (std::vector<std::string>{"203.0.113.9:19134 srflx 10.0.0.5:19132",
                                                              "2001:db8::9:19134 srflx :::19132", "10.0.0.5:19132"}));
}

TEST(GatherCandidatesTest, KeepsLoopbackOnlyWhenTheClientCameThatWay)
{
    EXPECT_EQ(
        describe(gatherCandidates(addressesOf({"127.0.0.1"}), 19134, addressesOf({"127.0.0.1", "10.0.0.5"}), 19134)),
        (std::vector<std::string>{"127.0.0.1:19134", "10.0.0.5:19134"}));
    EXPECT_EQ(describe(gatherCandidates({}, 19134, addressesOf({"127.0.0.1", "10.0.0.5"}), 19134)),
              (std::vector<std::string>{"10.0.0.5:19134"}));
}

TEST(GatherCandidatesTest, DropsDuplicatesAndAddressesNobodyCanReach)
{
    const auto candidates =
        gatherCandidates(addressesOf({"::ffff:203.0.113.9", "203.0.113.9", "0.0.0.0", "224.0.0.1", "169.254.1.1"}),
                         19134, addressesOf({"10.0.0.5", "10.0.0.5", "169.254.9.9"}), 19132);
    EXPECT_EQ(describe(candidates),
              (std::vector<std::string>{"203.0.113.9:19134 srflx 10.0.0.5:19132", "10.0.0.5:19132"}));
}

}  // namespace
}  // namespace endgate::nethernet
