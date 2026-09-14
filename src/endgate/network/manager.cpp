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
    : server_(server), logger_(LogManager::getLogger()), offline_(server),
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

    asio::ip::udp::socket socket(io_context_);
    const asio::ip::udp::endpoint game{address, port};
    if (socket.open(game.protocol(), ec) || socket.bind(game, ec)) {
        logger_.error("Can't bind to {}:{}: {}", address.to_string(), port, ec.message());
        return false;
    }

    auto &endpoint = endpoints_.emplace_back(std::move(socket));

    offline_.setPingResponder(port);

    logger_.info("Listening on {}", endpoint.getLocalAddress());

    asio::co_spawn(io_context_, receive(endpoint.getSocket()), asio::detached);
    return true;
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

asio::awaitable<void> ConnectionManager::receive(asio::ip::udp::socket &socket)
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

        logger_.debug("Received {} bytes from {}:{}", size, sender.address().to_string(), sender.port());

        if (const auto pong = offline_.handle(std::span{buffer}.first(size)); !pong.empty()) {
            co_await socket.async_send_to(pong.toBufferSequence(), sender, asio::as_tuple(asio::use_awaitable));
        }
    }
}

}  // namespace endgate
