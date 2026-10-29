#include "endgate/connection/player.h"

#include <utility>

#include "endgate/config.h"
#include "endgate/connection/backend.h"
#include "endgate/logging/manager.h"
#include "endgate/server.h"

namespace endgate {

Player::Player(ProxyServer &server, std::shared_ptr<MinecraftConnection> connection)
    : server_(server), logger_(LogManager::getLogger()), connection_(std::move(connection))
{
}

RegisteredServer *Player::getNextServerToTry() const
{
    const auto &order = server_.getConfiguration().getAttemptConnectionOrder();
    if (order.empty()) {
        logger_.warn("No servers configured to try; {} has nowhere to go", connection_->getRemoteAddress());
        return nullptr;
    }
    for (const auto &name : order) {
        if (auto *const server = server_.getServer(name)) {
            return server;
        }
    }
    logger_.warn("None of the servers in try are configured; {} has nowhere to go", connection_->getRemoteAddress());
    return nullptr;
}

void Player::createConnectionRequest(RegisteredServer &server)
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
    : logger_(LogManager::getLogger()), player_(std::move(player))
{
}

void ClientPlaySessionHandler::connected()
{
    connected_ = true;
    logger_.info("Connected: {}", player_->getConnection().getRemoteAddress());
    auto *const server = player_->getNextServerToTry();
    if (server == nullptr) {
        player_->disconnect();
        return;
    }
    player_->createConnectionRequest(*server);
}

void ClientPlaySessionHandler::handleUnknown(ByteBuf batch)
{
    const auto &server = player_->getConnectedServer();
    if (!server) {
        logger_.debug("Dropping {} bytes from {}: no backend", batch.length(),
                      player_->getConnection().getRemoteAddress());
        return;
    }
    server->write(std::move(batch));
}

void ClientPlaySessionHandler::disconnected()
{
    if (connected_) {
        logger_.info("Disconnected: {}", player_->getConnection().getRemoteAddress());
    }
    player_->teardown();
}

}  // namespace endgate
