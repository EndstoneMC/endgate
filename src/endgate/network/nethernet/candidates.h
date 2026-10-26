#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <asio/ip/address.hpp>

#include "endgate/network/rtc/description.h"

namespace endgate::nethernet {

/**
 * The candidates an answer offers. First the addresses the client reached the proxy at: a host
 * candidate when the address is this machine's own, a server-reflexive one when NAT stands between.
 * Then this machine's other addresses, for clients on the same network.
 *
 * @see NetherNet::PortRangeStunServer::Reflect
 * @see RFC 8445 section 5.1.1
 */
[[nodiscard]] std::vector<rtc::CandidateAddress> gatherCandidates(std::span<const asio::ip::address> reached,
                                                                  std::uint16_t reached_port,
                                                                  std::span<const asio::ip::address> interfaces,
                                                                  std::uint16_t local_port);

}  // namespace endgate::nethernet
