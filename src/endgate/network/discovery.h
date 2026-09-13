#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "endgate/network/buffer.h"

namespace endgate {

class ProxyServer;

/**
 * Answers NetherNet's LAN broadcast discovery. The whole exchange is sealed under a key derived from
 * the application id, and the announcement inside is hex rather than the semicolons RakNet uses.
 *
 * @see NetherNet::LanThreadManager
 * @see NetherNetServerLocator
 */
class LanDiscoveryHandler {
public:
    explicit LanDiscoveryHandler(ProxyServer &server);

    /**
     * @see NetherNetServerLocator::_cacheDiscoveryResponseData
     */
    void cacheDiscoveryResponseData();

    [[nodiscard]] ByteBuf handle(std::span<const std::byte> datagram) const;

private:
    /**
     * @see NetherNet::AesContext::Seal
     */
    [[nodiscard]] std::vector<std::byte> seal(std::span<const std::byte> plaintext) const;

    /**
     * @see NetherNet::AesContext::Open
     */
    [[nodiscard]] std::vector<std::byte> open(std::span<const std::byte> envelope) const;

    ProxyServer &server_;
    std::vector<std::uint8_t> key_;
    std::uint64_t network_id_;
    std::string cached_broadcast_discovery_response_;
};

}  // namespace endgate
