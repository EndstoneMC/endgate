#include "endgate/network/manager.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>

#include "endgate/logging/manager.h"
#include "endgate/server.h"

namespace endgate {

namespace {
using DatagramBuffer = std::array<std::byte, 65535>;
}  // namespace

ConnectionManager::ConnectionManager(ProxyServer &server)
    : server_(server), logger_(LogManager::getLogger()), offline_(server), lan_discovery_(server),
      work_guard_(asio::make_work_guard(io_context_)), signals_(io_context_, SIGINT, SIGTERM),
      threads_(std::max(1U, std::thread::hardware_concurrency()))
{
}

ConnectionManager::~ConnectionManager()
{
    shutdown();
    awaitTermination();
}

void ConnectionManager::logChannelInformation()
{
    logger_.info("Connections will use asio channels, {} worker threads", threads_);
}

bool ConnectionManager::bind(const asio::ip::address &address, std::uint16_t port)
{
    asio::error_code ec;

    asio::ip::tcp::acceptor acceptor(io_context_);
    const asio::ip::tcp::endpoint signaling{address, port};
    if (acceptor.open(signaling.protocol(), ec) || acceptor.set_option(asio::socket_base::reuse_address(true), ec) ||
        acceptor.bind(signaling, ec) || acceptor.listen(asio::socket_base::max_listen_connections, ec)) {
        logger_.error("Can't bind to {}:{}: {}", address.to_string(), port, ec.message());
        return false;
    }

    asio::ip::udp::socket socket(io_context_);
    const asio::ip::udp::endpoint game{address, port};
    if (socket.open(game.protocol(), ec) || socket.bind(game, ec)) {
        logger_.error("Can't bind to {}:{}: {}", address.to_string(), port, ec.message());
        return false;
    }

    auto &tcp_endpoint = endpoints_.emplace_back(std::move(acceptor), ListenerType::HttpSignaling);
    auto &udp_endpoint = endpoints_.emplace_back(std::move(socket), ListenerType::Gameplay);

    offline_.setPingResponder(port);

    logger_.info("Listening on {}", udp_endpoint.getLocalAddress());
    logger_.info("Listening for {} on {}", toString(ListenerType::HttpSignaling), tcp_endpoint.getLocalAddress());

    asio::co_spawn(io_context_, accept(tcp_endpoint.getAcceptor()), asio::detached);
    asio::co_spawn(io_context_, receive(udp_endpoint.getSocket()), asio::detached);
    return true;
}

void ConnectionManager::lanDiscoveryBind(const asio::ip::address &address)
{
    constexpr std::uint16_t lan_discovery_port = 7551;

    asio::error_code ec;

    asio::ip::udp::socket socket(io_context_);
    const asio::ip::udp::endpoint local{address, lan_discovery_port};
    if (socket.open(local.protocol(), ec) || socket.set_option(asio::socket_base::reuse_address(true), ec) ||
        socket.set_option(asio::socket_base::broadcast(true), ec) || socket.bind(local, ec)) {
        logger_.warn("Can't bind to {}:{}: {}. LAN discovery will be unavailable.", address.to_string(),
                     lan_discovery_port, ec.message());
        return;
    }

    lan_discovery_.cacheDiscoveryResponseData();

    auto &endpoint = endpoints_.emplace_back(std::move(socket), ListenerType::LanDiscovery);
    logger_.info("Listening for {} on {}", toString(ListenerType::LanDiscovery), endpoint.getLocalAddress());
    asio::co_spawn(io_context_, receiveLanDiscovery(endpoint.getSocket()), asio::detached);
}

void ConnectionManager::start()
{
    signals_.async_wait([this](const asio::error_code &ec, int) {
        if (!ec) {
            server_.shutdown(false);
        }
    });

    workers_.reserve(threads_);
    for (unsigned int i = 0; i < threads_; ++i) {
        workers_.emplace_back([this] { io_context_.run(); });
    }
}

void ConnectionManager::shutdown()
{
    for (auto &endpoint : endpoints_) {
        endpoint.close();
    }
    signals_.cancel();
    work_guard_.reset();
}

void ConnectionManager::awaitTermination()
{
    for (auto &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

asio::awaitable<void> ConnectionManager::accept(asio::ip::tcp::acceptor &acceptor)
{
    for (;;) {
        auto [ec, socket] = co_await acceptor.async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted) {
            co_return;
        }
        if (ec) {
            logger_.debug("Can't accept a signaling connection: {}", ec.message());
            continue;
        }

        asio::error_code ignored;
        const auto remote = socket.remote_endpoint(ignored);
        logger_.debug("Accepted a signaling connection from {}:{}", remote.address().to_string(), remote.port());
        socket.close(ignored);
    }
}

asio::awaitable<void> ConnectionManager::receive(asio::ip::udp::socket &socket)
{
    constexpr std::size_t stun_header_size = 20;
    constexpr std::uint32_t stun_magic_cookie = 0x2112A442;
    constexpr std::uint8_t dtls_content_type_min = 20;
    constexpr std::uint8_t dtls_content_type_max = 63;

    DatagramBuffer buffer{};
    asio::ip::udp::endpoint sender;

    for (;;) {
        auto [ec, size] =
            co_await socket.async_receive_from(asio::buffer(buffer), sender, asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted) {
            co_return;
        }
        if (ec) {
            continue;
        }

        const auto content_type = std::to_integer<std::uint8_t>(buffer[0]);
        const bool is_stun =
            size >= stun_header_size &&
            (std::to_integer<std::uint32_t>(buffer[4]) << 24U | std::to_integer<std::uint32_t>(buffer[5]) << 16U |
             std::to_integer<std::uint32_t>(buffer[6]) << 8U | std::to_integer<std::uint32_t>(buffer[7])) ==
                stun_magic_cookie;
        const bool is_dtls = content_type >= dtls_content_type_min && content_type <= dtls_content_type_max;

        logger_.debug("Received {} bytes of {} from {}:{}", size, is_stun || is_dtls ? "NetherNet" : "RakNet",
                      sender.address().to_string(), sender.port());

        if (is_stun || is_dtls) {
            continue;
        }

        if (const auto pong = offline_.handle(std::span{buffer}.first(size)); !pong.empty()) {
            co_await socket.async_send_to(pong.toBufferSequence(), sender, asio::as_tuple(asio::use_awaitable));
        }
    }
}

asio::awaitable<void> ConnectionManager::receiveLanDiscovery(asio::ip::udp::socket &socket)
{
    DatagramBuffer buffer{};
    asio::ip::udp::endpoint sender;

    for (;;) {
        auto [ec, size] =
            co_await socket.async_receive_from(asio::buffer(buffer), sender, asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted) {
            co_return;
        }
        if (ec) {
            continue;
        }

        logger_.debug("Received {} bytes of LAN discovery from {}:{}", size, sender.address().to_string(),
                      sender.port());

        if (const auto response = lan_discovery_.handle(std::span{buffer}.first(size)); !response.empty()) {
            co_await socket.async_send_to(response.toBufferSequence(), sender, asio::as_tuple(asio::use_awaitable));
        }
    }
}

}  // namespace endgate
