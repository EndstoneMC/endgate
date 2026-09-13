#include "endgate/network/ping.h"

#include <string>

#include "endgate/config.h"
#include "endgate/network/version.h"
#include "endgate/server.h"

namespace endgate {

ServerPing ServerListPingHandler::getInitialPing() const
{
    const auto &configuration = server_.getConfiguration();
    return {ServerPing::Version{MAXIMUM_VERSION.getProtocol(), std::string{MAXIMUM_VERSION.getName()}},
            ServerPing::Players{0, configuration.getShowMaxPlayers()}, configuration.getMotd(),
            server_.getVersion().getName(), GameType::Survival};
}

}  // namespace endgate
