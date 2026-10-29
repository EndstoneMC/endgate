#include "endgate/network/endpoint.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include "endgate/connection/player.h"
#include "endgate/connection/raknet.h"
#include "endgate/logging/manager.h"

namespace endgate {

Endpoint::Endpoint(ProxyServer &server, asio::io_context &io_context, std::shared_ptr<asio::ip::udp::socket> socket,
                   std::uint64_t guid, std::span<const std::byte> offline_ping_response)
    : server_(server), logger_(LogManager::getLogger()), io_context_(io_context), socket_(std::move(socket)),
      strand_(asio::make_strand(io_context)), offline_(*this, guid)
{
    offline_.setOfflinePingResponse(offline_ping_response);
}

Endpoint::~Endpoint() = default;

std::string Endpoint::getLocalAddress() const
{
    return toString(socket_->local_endpoint());
}

void Endpoint::start()
{
    asio::co_spawn(strand_, receive(), asio::detached);
}

asio::awaitable<void> Endpoint::receive()
{
    std::array<std::byte, 65536> buffer{};
    asio::ip::udp::endpoint sender;
    for (;;) {
        const auto [ec, size] =
            co_await socket_->async_receive_from(asio::buffer(buffer), sender, asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted || closed_) {
            co_return;
        }
        if (ec || size == 0) {
            continue;
        }
        handleDatagram(std::span{buffer}.first(size), sender);
    }
}

void Endpoint::sendTo(const raknet::SystemAddress &address, std::span<const std::byte> datagram)
{
    asio::error_code ignored;
    socket_->send_to(asio::buffer(datagram), address, 0, ignored);
}

void Endpoint::handleDatagram(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender)
{
    const auto id = static_cast<raknet::MessageId>(datagram.front());
    const bool offline_message = raknet::isOfflineMessage(datagram);
    if (offline_message &&
        (id == raknet::MessageId::UnconnectedPing || id == raknet::MessageId::UnconnectedPingOpenConnections ||
         id == raknet::MessageId::OpenConnectionRequest1)) {
        static_cast<void>(offline_.handle(datagram, sender, true));
        return;
    }
    if (const auto route = routes_.find(sender); route != routes_.end()) {
        route->second->receive(datagram, sender);
        return;
    }
    if (offline_message && id == raknet::MessageId::OpenConnectionRequest2) {
        accept(datagram, sender);
    }
}

void Endpoint::accept(std::span<const std::byte> datagram, const raknet::SystemAddress &sender)
{
    constexpr std::size_t half_open_limit = 1024;

    const auto request = offline_.handle(datagram, sender, true);
    if (!request) {
        return;
    }
    const auto guid = request->client_guid == raknet::UNASSIGNED_RAKNET_GUID
                        ? std::nullopt
                        : std::optional<std::uint64_t>{request->client_guid};
    if (guid && guids_.contains(*guid)) {
        offline_.reject(sender, raknet::MessageId::AlreadyConnected);
        return;
    }
    if (half_open_.size() >= half_open_limit) {
        const auto oldest = half_open_.begin()->second;
        half_open_.erase(half_open_.begin());
        if (const auto registration = connections_.find(oldest); registration != connections_.end()) {
            registration->second.half_open.reset();
            unroute(oldest, registration->second);
        }
        oldest->shutdown();
    }

    const auto system_index = getFreeSystemIndex();
    const auto connection = std::make_shared<RakNetConnection>(*this, sender, *request, offline_.getGuid(),
                                                               system_index, raknet::Clock::now());
    connection->setSessionHandler(
        std::make_shared<ClientPlaySessionHandler>(std::make_shared<Player>(server_, connection)));
    const auto half_open = next_half_open_++;
    connections_.emplace(
        connection,
        Registration{.routes = {sender}, .guid = guid, .system_index = system_index, .half_open = half_open});
    if (guid) {
        guids_.insert(*guid);
    }
    half_open_.emplace(half_open, connection);
    routes_.insert_or_assign(sender, connection);
    connection->receive(datagram, sender);
}

std::uint16_t Endpoint::getFreeSystemIndex() const
{
    std::vector<bool> used(connections_.size() + 1);
    for (const auto &[connection, registration] : connections_) {
        if (registration.system_index && *registration.system_index < used.size()) {
            used[*registration.system_index] = true;
        }
    }
    return static_cast<std::uint16_t>(std::ranges::find(used, false) - used.begin());
}

void Endpoint::unroute(const std::shared_ptr<MinecraftConnection> &connection, Registration &registration)
{
    for (const auto &remote : std::exchange(registration.routes, {})) {
        if (const auto route = routes_.find(remote); route != routes_.end() && route->second == connection) {
            routes_.erase(route);
        }
    }
}

void Endpoint::registerConnection(std::shared_ptr<MinecraftConnection> connection, std::optional<std::uint64_t> guid)
{
    asio::post(strand_, [this, connection = std::move(connection), guid] {
        if (closed_) {
            connection->shutdown();
            return;
        }
        connections_.emplace(connection, Registration{.guid = guid});
        if (guid) {
            guids_.insert(*guid);
        }
    });
}

void Endpoint::unregisterConnection(std::shared_ptr<MinecraftConnection> connection)
{
    asio::post(strand_, [this, connection = std::move(connection)] {
        const auto registration = connections_.find(connection);
        if (registration == connections_.end()) {
            return;
        }
        unroute(connection, registration->second);
        if (registration->second.guid) {
            guids_.erase(*registration->second.guid);
        }
        if (registration->second.half_open) {
            half_open_.erase(*registration->second.half_open);
        }
        connections_.erase(registration);
    });
}

void Endpoint::activated(std::shared_ptr<MinecraftConnection> connection)
{
    asio::post(strand_, [this, connection = std::move(connection)] {
        const auto registration = connections_.find(connection);
        if (registration == connections_.end() || !registration->second.half_open) {
            return;
        }
        half_open_.erase(*registration->second.half_open);
        registration->second.half_open.reset();
    });
}

void Endpoint::addRoute(const asio::ip::udp::endpoint &remote, std::shared_ptr<MinecraftConnection> connection)
{
    asio::post(strand_, [this, remote, connection = std::move(connection)] {
        const auto registration = connections_.find(connection);
        if (registration == connections_.end()) {
            return;
        }
        if (!std::ranges::contains(registration->second.routes, remote)) {
            registration->second.routes.push_back(remote);
        }
        routes_.insert_or_assign(remote, connection);
    });
}

void Endpoint::removeRoute(const asio::ip::udp::endpoint &remote, std::shared_ptr<MinecraftConnection> connection)
{
    asio::post(strand_, [this, remote, connection = std::move(connection)] {
        const auto registration = connections_.find(connection);
        if (registration == connections_.end()) {
            return;
        }
        std::erase(registration->second.routes, remote);
        if (const auto route = routes_.find(remote); route != routes_.end() && route->second == connection) {
            routes_.erase(route);
        }
    });
}

void Endpoint::close()
{
    closed_ = true;
    asio::error_code ignored;
    std::ignore = socket_->cancel(ignored);
    for (const auto &[connection, registration] : std::exchange(connections_, {})) {
        connection->shutdown();
    }
    routes_.clear();
    guids_.clear();
    half_open_.clear();
}

}  // namespace endgate
