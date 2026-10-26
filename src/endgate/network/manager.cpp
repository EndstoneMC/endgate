#include "endgate/network/manager.h"

#include <algorithm>
#include <utility>

#include <asio/dispatch.hpp>

#include "endgate/config.h"
#include "endgate/logging/manager.h"
#include "endgate/server.h"

namespace endgate {

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
    if (socket.open(game.protocol(), ec) || socket.bind(game, ec) || socket.non_blocking(true, ec)) {
        logger_.error("Can't bind to {}:{}: {}", address.to_string(), port, ec.message());
        return false;
    }

    offline_.setPingResponder(port);
    auto &endpoint = endpoints_.emplace_back(server_, io_context_, std::move(socket), offline_.getGuid(),
                                             offline_.getOfflinePingResponse());

    logger_.info("Listening on {}", endpoint.getLocalAddress());
    asio::dispatch(endpoint.getExecutor(), [&endpoint] { endpoint.start(); });
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
        asio::dispatch(endpoint.getExecutor(), [&endpoint] { endpoint.close(); });
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

}  // namespace endgate
