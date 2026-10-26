#include "endgate/network/endpoint.h"

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>

#include "endgate/config.h"
#include "endgate/logging/manager.h"
#include "endgate/network/legacy/raknet/PacketPriority.h"
#include "endgate/network/legacy/raknet/RakPeer.h"
#include "endgate/server.h"

namespace endgate {

Endpoint::Endpoint(ProxyServer &server, asio::io_context &io_context, asio::ip::udp::socket socket)
    : server_(server), logger_(LogManager::getLogger()), socket_(std::move(socket)),
      strand_(asio::make_strand(io_context))
{
}

std::string Endpoint::getLocalAddress() const
{
    const auto local = socket_.local_endpoint();
    return local.address().to_string() + ":" + std::to_string(local.port());
}

asio::ip::udp::endpoint Endpoint::toAsio(const RakNet::SystemAddress &address)
{
    const auto size = address.address.sa_stor.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    asio::ip::udp::endpoint endpoint;
    endpoint.resize(size);
    std::memcpy(endpoint.data(), &address.address.sa_stor, size);
    return endpoint;
}

RakNet::SystemAddress Endpoint::fromAsio(const asio::ip::udp::endpoint &endpoint)
{
    RakNet::SystemAddress address;
    std::memcpy(&address.address.sa_stor, endpoint.data(), endpoint.size());
    address.debugPort = endpoint.port();
    return address;
}

void Endpoint::send(const RakNet::SystemAddress &peer, const char *data, int length)
{
    auto bytes = std::make_shared<std::vector<char>>(data, data + length);
    socket_.async_send_to(asio::buffer(*bytes), toAsio(peer), [bytes](const asio::error_code &, std::size_t) {});
}

void Endpoint::send(const RakNet::SystemAddress &peer, const ByteBuf &bytes)
{
    auto copy = std::make_shared<std::vector<std::byte>>(bytes.length());
    bytes.copyTo(*copy);
    socket_.async_send_to(asio::buffer(*copy), toAsio(peer), [copy](const asio::error_code &, std::size_t) {});
}

void Endpoint::channelActive(const RakNet::SystemAddress &peer, const RakNet::RakNetGUID &guid)
{
    const std::string key = peer.ToString(true);
    addresses_[key] = peer;

    // A backend we opened for a player who is already waiting on it.
    if (!awaiting_backend_.empty()) {
        counterpart_[awaiting_backend_] = key;
        counterpart_[key] = awaiting_backend_;
        logger_.info("Player {} is now on backend {}", awaiting_backend_, key);
        awaiting_backend_.clear();
        return;
    }

    logger_.info("Connected: {}", key);

    const auto &order = server_.getConfiguration().getAttemptConnectionOrder();
    const auto &servers = server_.getConfiguration().getServers();
    if (order.empty()) {
        logger_.warn("No servers configured to try; {} has nowhere to go", key);
        return;
    }
    const auto entry = servers.find(order.front());
    if (entry == servers.end()) {
        logger_.warn("Server \"{}\" is in try but not configured", order.front());
        return;
    }

    const auto colon = entry->second.rfind(':');
    const auto host = entry->second.substr(0, colon);
    const auto port = static_cast<unsigned short>(std::stoi(entry->second.substr(colon + 1)));

    awaiting_backend_ = key;
    if (peer_->Connect(host.c_str(), port, nullptr, 0) != RakNet::CONNECTION_ATTEMPT_STARTED) {
        logger_.warn("Can't reach backend {} for {}", entry->second, key);
        awaiting_backend_.clear();
        return;
    }
    logger_.info("Opening backend {} for {}", entry->second, key);
}

void Endpoint::channelRead(const RakNet::SystemAddress &peer, const unsigned char *data, unsigned int length)
{
    const auto pairing = counterpart_.find(peer.ToString(true));
    if (pairing == counterpart_.end()) {
        logger_.debug("Dropping {} bytes from {}: no counterpart yet", length, peer.ToString(true));
        return;
    }

    const auto target = addresses_.find(pairing->second);
    if (target == addresses_.end()) {
        return;
    }

    logger_.debug("Forwarding {} bytes {} -> {}", length, peer.ToString(true), pairing->second);
    peer_->Send(reinterpret_cast<const char *>(data), static_cast<int>(length), HIGH_PRIORITY, RELIABLE_ORDERED, 0,
                target->second, false);
}

void Endpoint::onReliabilityLayerNotification(const RakNet::SystemAddress &peer, const char *errorMessage, bool isError)
{
    if (isError) {
        logger_.warn("Malformed datagram from {}:{}: {}", peer.ToString(false), peer.GetPort(), errorMessage);
    }
}

void Endpoint::channelInactive(const RakNet::SystemAddress &peer, DisconnectReason reason)
{
    const std::string key = peer.ToString(true);
    logger_.info("Disconnected: {}", key);

    if (const auto pairing = counterpart_.find(key); pairing != counterpart_.end()) {
        const auto other = pairing->second;
        counterpart_.erase(pairing);
        counterpart_.erase(other);
        if (const auto target = addresses_.find(other); target != addresses_.end()) {
            peer_->CloseConnection(target->second, true);
        }
    }
    addresses_.erase(key);
    if (awaiting_backend_ == key) {
        awaiting_backend_.clear();
    }
}

void Endpoint::close()
{
    asio::error_code ignored;
    socket_.close(ignored);
}

}  // namespace endgate
