#include "endgate/network/manager.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <system_error>
#include <utility>

#include <asio/dispatch.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/ssl/context.hpp>

#include "endgate/config.h"
#include "endgate/logging/manager.h"
#include "endgate/server.h"

namespace endgate {

ConnectionManager::ConnectionManager(ProxyServer &server)
    : server_(server), logger_(LogManager::getLogger("endgate::ConnectionManager")), offline_(server),
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
    logger_.info("Connections will use {} channels, {} compression, {} ciphers", "asio", "libdeflate", "OpenSSL");
}

bool ConnectionManager::bind(const asio::ip::address &address, std::uint16_t port)
{
    asio::error_code ec;

    auto socket = std::make_shared<asio::ip::udp::socket>(io_context_);
    const asio::ip::udp::endpoint game{address, port};
    const bool dual_stack = address.is_v6() && address.is_unspecified();
    if (socket->open(game.protocol(), ec) || (dual_stack && socket->set_option(asio::ip::v6_only(false), ec)) ||
        socket->bind(game, ec) || socket->non_blocking(true, ec)) {
        logger_.error("Can't bind to {}:{}: {}", address.to_string(), port, ec.message());
        return false;
    }

    constexpr auto identity_key_path = "keys/server_identity_key.pem";

    const auto &configuration = server_.getConfiguration();
    std::shared_ptr<asio::ssl::context> tls;
    try {
        if (!identity_) {
            identity_ = std::make_shared<const nethernet::ServerIdentityAssertionGenerator>(
                nethernet::ServerIdentityAssertionGenerator::loadOrCreateKey(identity_key_path),
                nethernet::ServerIdentityAssertionGenerator::Config{});
        }
        if (!configuration.getTlsCertificate().empty()) {
            tls = nethernet::HttpSignalingServer::loadTls(configuration.getTlsCertificate(),
                                                          configuration.getTlsCertificateKey());
        }
    }
    catch (const std::exception &e) {
        logger_.error("Can't set up NetherNet signalling: {}", e.what());
        return false;
    }

    const auto local_port = socket->local_endpoint().port();
    offline_.setPingResponder(local_port);
    auto &endpoint = endpoints_.emplace_back(server_, io_context_, socket, offline_.getGuid(),
                                             offline_.getOfflinePingResponse(), identity_, std::move(tls));
    try {
        endpoint.listen({address, local_port});
    }
    catch (const std::system_error &e) {
        logger_.error("Can't bind to {}:{}: {}", address.to_string(), local_port, e.code().message());
        endpoints_.pop_back();
        return false;
    }

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
