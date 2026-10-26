#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace endgate {

class ProxyServer;

/**
 * Builds what a listener announces to an unconnected ping. The RakNet engine answers the ping itself,
 * with these bytes.
 *
 * @see RakNetServerLocator
 */
class OfflineMessageHandler {
public:
    explicit OfflineMessageHandler(ProxyServer &server);

    /**
     * @see RakNetServerLocator::_setPingResponder
     */
    void setPingResponder(std::uint16_t port);

    [[nodiscard]] std::uint64_t getGuid() const { return guid_; }
    [[nodiscard]] std::span<const std::byte> getOfflinePingResponse() const { return offline_ping_response_; }

private:
    ProxyServer &server_;
    std::uint64_t guid_;
    std::vector<std::byte> offline_ping_response_;
};

}  // namespace endgate
