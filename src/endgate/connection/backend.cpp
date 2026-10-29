#include "endgate/connection/backend.h"

#include <chrono>
#include <optional>
#include <random>
#include <string>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/cancel_after.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/udp.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include "endgate/connection/player.h"
#include "endgate/connection/raknet.h"
#include "endgate/logging/manager.h"
#include "endgate/network/endpoint.h"
#include "endgate/server/registered.h"

namespace endgate {

ServerConnection::ServerConnection(RegisteredServer &server, std::shared_ptr<Player> player)
    : server_(server), player_(std::move(player)), logger_(LogManager::getLogger())
{
}

void ServerConnection::connect()
{
    asio::co_spawn(player_->getConnection().getExecutor(), open(shared_from_this()), asio::detached);
}

asio::awaitable<void> ServerConnection::open(std::shared_ptr<ServerConnection> self)
{
    constexpr std::chrono::seconds resolve_timeout{2};

    const auto &info = self->server_.getServerInfo();
    auto &player = *self->player_;
    const auto &player_address = player.getConnection().getRemoteAddress();

    std::optional<asio::ip::udp::endpoint> address;
    asio::error_code ec;
    if (const auto literal = asio::ip::make_address(info.getHost(), ec); !ec) {
        address.emplace(literal, info.getPort());
    }
    else {
        asio::ip::udp::resolver resolver(co_await asio::this_coro::executor);
        const auto [error, results] =
            co_await resolver.async_resolve(info.getHost(), std::to_string(info.getPort()),
                                            asio::cancel_after(resolve_timeout, asio::as_tuple(asio::use_awaitable)));
        if (!error && !results.empty()) {
            address = results.begin()->endpoint();
        }
    }

    std::optional<ServerInfo::Transport> transport;
    if (address && player.isActive()) {
        transport = co_await self->server_.detectTransport(*address);
    }
    if (!player.isActive()) {
        co_return;
    }
    if (!address || !transport) {
        self->logger_.warn("Can't reach backend {} for {}", info.getAddress(), player_address);
        player.disconnect();
        co_return;
    }
    if (*transport == ServerInfo::Transport::NetherNet) {
        self->logger_.warn("Backend {} speaks NetherNet, which endgate can't connect to yet; disconnecting {}",
                           info.getAddress(), player_address);
        player.disconnect();
        co_return;
    }

    auto &endpoint = player.getConnection().getEndpoint();
    const auto guid = std::mt19937_64{std::random_device{}()}();
    const auto connection = std::make_shared<RakNetConnection>(endpoint, *address, guid, raknet::Clock::now());
    connection->setSessionHandler(std::make_shared<BackendPlaySessionHandler>(self));
    self->connection_ = connection;
    endpoint.registerConnection(connection, guid);
    self->logger_.info("Opening backend {} for {}", info.getAddress(), player_address);
    connection->connect();
}

void ServerConnection::disconnect()
{
    pending_.clear();
    pending_bytes_ = 0;
    if (connection_) {
        connection_->close();
    }
}

void ServerConnection::write(ByteBuf batch)
{
    constexpr std::size_t max_pending_bytes = 64 * 1024;

    if (connected_) {
        connection_->write(std::move(batch));
        return;
    }
    if (pending_bytes_ > max_pending_bytes) {
        return;
    }
    pending_bytes_ += batch.length();
    if (pending_bytes_ > max_pending_bytes) {
        logger_.warn("{} sent more than {} bytes before backend {} was up; disconnecting",
                     player_->getConnection().getRemoteAddress(), max_pending_bytes,
                     server_.getServerInfo().getAddress());
        pending_.clear();
        player_->disconnect();
        return;
    }
    pending_.push_back(std::move(batch));
}

void ServerConnection::connected()
{
    if (!player_->isActive() || !connection_) {
        return;
    }
    connected_ = true;
    for (auto &batch : std::exchange(pending_, {})) {
        connection_->write(std::move(batch));
    }
    pending_bytes_ = 0;
}

BackendPlaySessionHandler::BackendPlaySessionHandler(std::shared_ptr<ServerConnection> server_connection)
    : logger_(LogManager::getLogger()), server_connection_(std::move(server_connection))
{
}

void BackendPlaySessionHandler::connected()
{
    connected_ = true;
    auto &player = server_connection_->getPlayer().getConnection();
    logger_.info("Player {} is now on backend {}", player.getRemoteAddress(),
                 server_connection_->getConnection()->getRemoteAddress());
    asio::post(player.getExecutor(), [server_connection = server_connection_] { server_connection->connected(); });
}

void BackendPlaySessionHandler::handleUnknown(ByteBuf batch)
{
    server_connection_->getPlayer().getConnection().write(std::move(batch));
}

void BackendPlaySessionHandler::disconnected()
{
    logger_.info("Disconnected: {}", server_connection_->getConnection()->getRemoteAddress());
    if (!connected_) {
        server_connection_->getServer().forgetTransport();
    }
    server_connection_->getPlayer().disconnect();
}

}  // namespace endgate
