#include "endgate/network/offline.h"

#include <optional>
#include <random>
#include <string>

#include "endgate/network/ping.h"
#include "endgate/server.h"

namespace endgate {

OfflineMessageHandler::OfflineMessageHandler(ProxyServer &server)
    : server_(server), guid_(std::mt19937_64{std::random_device{}()}())
{
}

void OfflineMessageHandler::setPingResponder(std::uint16_t port)
{
    const auto announcement = server_.getServerListPingHandler().getInitialPing(std::nullopt).toMotd(guid_, port);

    const auto length = static_cast<std::uint16_t>(announcement.size());
    offline_ping_response_.clear();
    offline_ping_response_.push_back(static_cast<std::byte>(length >> 8U));
    offline_ping_response_.push_back(static_cast<std::byte>(length & 0xFFU));
    for (const auto character : std::string_view{announcement}.substr(0, length)) {
        offline_ping_response_.push_back(static_cast<std::byte>(character));
    }
}

}  // namespace endgate
