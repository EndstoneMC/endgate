#include "endgate/connection/handshake.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "endgate/config.h"
#include "endgate/connection/login.h"
#include "endgate/network/version.h"
#include "endgate/server.h"

namespace endgate {

HandshakeSessionHandler::HandshakeSessionHandler(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection,
                                                 std::optional<GameServerToken> offer_identity)
    : server_(server), connection_(std::move(connection)), offer_identity_(std::move(offer_identity))
{
}

void HandshakeSessionHandler::connected()
{
    connection_->setState(StateRegistry::Login);
}

bool HandshakeSessionHandler::handle(const bp::RequestNetworkSettingsPacket &packet)
{
    const auto version = getProtocolVersion(packet.client_network_version);
    if (!version) {
        connection_->write(bp::PlayStatusPacket{.status = packet.client_network_version > MAXIMUM_VERSION.getProtocol()
                                                            ? bp::PlayStatus::LoginFailedServerOld
                                                            : bp::PlayStatus::LoginFailedClientOld});
        connection_->close();
        return true;
    }

    connection_->setProtocolVersion(*version);
    const auto threshold = server_.getConfiguration().getCompressionThreshold();
    const auto algorithm = threshold < 0 ? PacketCompressionAlgorithm::None : PacketCompressionAlgorithm::ZLib;
    const auto compression_threshold = static_cast<std::uint16_t>(std::max(threshold, 0));
    connection_->write(
        bp::NetworkSettingsPacket{.compression_threshold = compression_threshold, .compression_algorithm = algorithm});
    connection_->setCompressionThreshold(compression_threshold, algorithm);
    connection_->setSessionHandler(
        std::make_shared<InitialLoginSessionHandler>(server_, connection_, std::move(offer_identity_)));
    return true;
}

void HandshakeSessionHandler::handleUnknown(ByteBufView)
{
    connection_->close();
}

}  // namespace endgate
