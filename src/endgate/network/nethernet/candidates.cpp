#include "endgate/network/nethernet/candidates.h"

#include <algorithm>
#include <optional>

#include <asio/ip/udp.hpp>

namespace endgate::nethernet {

std::vector<rtc::CandidateAddress> gatherCandidates(std::span<const asio::ip::address> reached,
                                                    std::uint16_t reached_port,
                                                    std::span<const asio::ip::address> interfaces,
                                                    std::uint16_t local_port)
{
    const auto unmapped = [](const asio::ip::address &address) {
        return address.is_v6() && address.to_v6().is_v4_mapped()
                 ? asio::ip::address{asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6())}
                 : address;
    };
    const auto announceable = [](const asio::ip::address &address) {
        if (address.is_unspecified() || address.is_multicast()) {
            return false;
        }
        if (address.is_v4()) {
            const auto bytes = address.to_v4().to_bytes();
            return !(bytes[0] == 169 && bytes[1] == 254);
        }
        return !address.to_v6().is_link_local();
    };

    std::vector<asio::ip::address> local;
    for (const auto &address : interfaces) {
        const auto normalised = unmapped(address);
        if (announceable(normalised) && !std::ranges::contains(local, normalised)) {
            local.push_back(normalised);
        }
    }

    std::vector<rtc::CandidateAddress> candidates;
    std::vector<asio::ip::address> listed;
    for (const auto &address : reached) {
        const auto normalised = unmapped(address);
        if (!announceable(normalised) || std::ranges::contains(listed, normalised)) {
            continue;
        }
        listed.push_back(normalised);
        if (std::ranges::contains(local, normalised)) {
            candidates.push_back({.address = {normalised, reached_port}});
            continue;
        }
        const auto base = std::ranges::find_if(local, [&](const asio::ip::address &candidate) {
            return candidate.is_v4() == normalised.is_v4() && !candidate.is_loopback();
        });
        auto base_address = normalised.is_v4() ? asio::ip::address{asio::ip::address_v4::any()}
                                               : asio::ip::address{asio::ip::address_v6::any()};
        if (base != local.end()) {
            base_address = *base;
        }
        candidates.push_back(
            {.address = {normalised, reached_port}, .base = asio::ip::udp::endpoint{base_address, local_port}});
    }

    for (const auto &address : local) {
        if (address.is_loopback() || std::ranges::contains(listed, address)) {
            continue;
        }
        listed.push_back(address);
        candidates.push_back({.address = {address, local_port}});
    }
    return candidates;
}

}  // namespace endgate::nethernet
