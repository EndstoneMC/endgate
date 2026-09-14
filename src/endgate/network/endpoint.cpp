#include "endgate/network/endpoint.h"

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>

#include "endgate/logging/manager.h"
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
    logger_.info("Connected: {}:{}", peer.ToString(false), peer.GetPort());
}

void Endpoint::channelRead(const RakNet::SystemAddress &peer, const unsigned char *data, unsigned int length)
{
    logger_.debug("Read {} bytes from {}:{}", length, peer.ToString(false), peer.GetPort());
}

void Endpoint::onReliabilityLayerNotification(const RakNet::SystemAddress &peer, const char *errorMessage, bool isError)
{
    if (isError) {
        logger_.warn("Malformed datagram from {}:{}: {}", peer.ToString(false), peer.GetPort(), errorMessage);
    }
}

void Endpoint::channelInactive(const RakNet::SystemAddress &peer, DisconnectReason reason)
{
    logger_.info("Disconnected: {}:{}", peer.ToString(false), peer.GetPort());
}

void Endpoint::close()
{
    asio::error_code ignored;
    socket_.close(ignored);
}

}  // namespace endgate
