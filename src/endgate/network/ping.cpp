#include "endgate/network/ping.h"

#include <string>

#include "endgate/config.h"
#include "endgate/network/version.h"
#include "endgate/server.h"

namespace endgate {

ServerPing ServerListPingHandler::getInitialPing(std::optional<int> protocol) const
{
    const auto &configuration = server_.getConfiguration();
    const auto shown = protocol.and_then(getProtocolVersion).value_or(MAXIMUM_VERSION);
    return {ServerPing::Version{shown.getProtocol(), std::string{shown.getName()}},
            ServerPing::Players{0, configuration.getShowMaxPlayers()}, configuration.getMotd(),
            ProxyServer::getVersion().getName(), GameType::Survival};
}

}  // namespace endgate
