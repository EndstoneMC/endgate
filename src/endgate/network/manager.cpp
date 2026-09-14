#include "endgate/network/manager.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/bind_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/dispatch.hpp>
#include <asio/use_awaitable.hpp>

#include "endgate/config.h"
#include "endgate/logging/manager.h"
#include "endgate/server.h"

namespace endgate {

namespace {
using DatagramBuffer = std::array<std::byte, MAXIMUM_MTU_SIZE>;
}  // namespace

ConnectionManager::ConnectionManager(ProxyServer &server)
    : server_(server), logger_(LogManager::getLogger()), offline_(server),
      work_guard_(asio::make_work_guard(io_context_)), signals_(io_context_, SIGINT, SIGTERM),
      threads_(std::max(1U, std::thread::hardware_concurrency())), timer_(io_context_)
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

    auto &endpoint = endpoints_.emplace_back(server_, io_context_, std::move(socket));

    peer_.SetEndpoint(&endpoint);
    if (peer_.Startup(server_.getConfiguration().getShowMaxPlayers()) != RakNet::RAKNET_STARTED) {
        logger_.error("RakNet failed to start");
        return false;
    }
    peer_.SetMaximumIncomingConnections(server_.getConfiguration().getShowMaxPlayers());
    peer_.SetOfflinePingResponse(nullptr, 0);

    offline_.setPingResponder(port);

    logger_.info("Listening on {}", endpoint.getLocalAddress());

    asio::co_spawn(endpoint.getExecutor(), receive(endpoint), asio::detached);
    asio::dispatch(endpoint.getExecutor(), [this] { tick(); });
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
        asio::dispatch(endpoint.getExecutor(), [this, &endpoint] {
            stopping_ = true;
            timer_.cancel();
            endpoint.close();
        });
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

void ConnectionManager::tick()
{
    if (stopping_) {
        return;
    }

    peer_.RunUpdateCycle(scratch_);

    timer_.expires_after(TICK_INTERVAL);
    timer_.async_wait([this](const asio::error_code &ec) {
        if (!ec) {
            tick();
        }
    });
}

asio::awaitable<void> ConnectionManager::receive(Endpoint &endpoint)
{
    DatagramBuffer buffer{};
    asio::ip::udp::endpoint sender;

    for (;;) {
        auto [ec, size] = co_await endpoint.getSocket().async_receive_from(asio::buffer(buffer), sender,
                                                                           asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::operation_aborted) {
            co_return;
        }
        if (ec || size == 0) {
            continue;
        }

        const auto datagram = std::span{buffer}.first(size);

        // The unconnected surface is endgate's: RakPeer answers only what belongs to a connection.
        if (const auto pong = offline_.handle(datagram); !pong.empty()) {
            endpoint.send(Endpoint::fromAsio(sender), pong);
            continue;
        }

        peer_.OnDatagram(Endpoint::fromAsio(sender), reinterpret_cast<const char *>(datagram.data()),
                         static_cast<int>(size), scratch_);
    }
}

}  // namespace endgate
