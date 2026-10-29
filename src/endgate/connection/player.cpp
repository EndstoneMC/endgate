#include "endgate/connection/player.h"

#include <utility>

#include "endgate/config.h"
#include "endgate/connection/backend.h"
#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"
#include "endgate/server.h"

namespace endgate {

Player::Player(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection)
    : server_(server), logger_(LogManager::getLogger("Player")), connection_(std::move(connection))
{
}

BackendServer *Player::getNextServerToTry() const
{
    const auto &order = server_.getConfiguration().getAttemptConnectionOrder();
    if (order.empty()) {
        logger_.warn("[initial connection] {}: no available servers, try is empty", connection_->getRemoteAddress());
        return nullptr;
    }
    for (const auto &name : order) {
        if (auto *const server = server_.getServer(name)) {
            return server;
        }
    }
    logger_.warn("[initial connection] {}: no available servers, none of try is configured",
                 connection_->getRemoteAddress());
    return nullptr;
}

void Player::createConnectionRequest(BackendServer &server)
{
    connected_server_ = std::make_shared<ServerConnection>(server, shared_from_this());
    connected_server_->connect();
}

void Player::disconnect()
{
    connection_->close();
}

void Player::teardown()
{
    active_ = false;
    if (const auto server = std::exchange(connected_server_, nullptr)) {
        server->disconnect();
    }
}

ClientPlaySessionHandler::ClientPlaySessionHandler(std::shared_ptr<Player> player)
    : logger_(LogManager::getLogger("ClientPlaySessionHandler")), player_(std::move(player))
{
}

void ClientPlaySessionHandler::connected()
{
    connected_ = true;
    if (player_->getConnection().getEndpoint().getServer().getConfiguration().isLogPlayerConnections()) {
        logger_.info("[initial connection] {} has connected", player_->getConnection().getRemoteAddress());
    }
    auto *const server = player_->getNextServerToTry();
    if (server == nullptr) {
        player_->disconnect();
        return;
    }
    player_->createConnectionRequest(*server);
}

void ClientPlaySessionHandler::handleUnknown(ByteBufView batch)
{
    const auto &server = player_->getConnectedServer();
    if (!server) {
        logger_.debug("[initial connection] {}: dropping {} bytes, no server connection",
                      player_->getConnection().getRemoteAddress(), batch.readableBytes());
        return;
    }
    server->write(batch.copy());
}

void ClientPlaySessionHandler::readCompleted()
{
    if (const auto &server = player_->getConnectedServer()) {
        server->flush();
    }
}

void ClientPlaySessionHandler::disconnected()
{
    if (connected_ && player_->getConnection().getEndpoint().getServer().getConfiguration().isLogPlayerConnections()) {
        logger_.info("[initial connection] {} has disconnected", player_->getConnection().getRemoteAddress());
    }
    player_->teardown();
}

}  // namespace endgate
