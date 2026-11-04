#include "endgate/network/manager.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <latch>
#include <memory>
#include <ranges>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/detail/socket_option.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/ssl/context.hpp>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include "endgate/config.h"
#include "endgate/logging/manager.h"
#include "endgate/server.h"

namespace endgate {

ConnectionManager::ConnectionManager(ProxyServer &server)
    : server_(server), logger_(LogManager::getLogger(*this)), offline_(server), query_(server),
      group_(std::max(1U, std::thread::hardware_concurrency()), "IO Worker"),
      signals_(group_.begin()->getIoContext(), SIGINT, SIGTERM)
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

HttpClient ConnectionManager::createHttpClient()
{
    return HttpClient(group_.next(), {});
}

bool ConnectionManager::bind(asio::ip::address address, std::uint16_t port)
{
    if (address.is_v6() && address.is_unspecified()) {
        asio::error_code ec;
        asio::ip::udp::socket probe(group_.begin()->getExecutor());
        if (probe.open(asio::ip::udp::v6(), ec) || probe.set_option(asio::ip::v6_only(false), ec)) {
            logger_.warn("IPv6 is not available, so the proxy listens on IPv4 only: {}", ec.message());
            address = asio::ip::address_v4::any();
        }
    }

#ifdef SO_REUSEPORT
    using ReusePort = asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>;
    const auto binds = group_.size();
#else
    const std::size_t binds = 1;
#endif
#ifdef _WIN32
    using ListenerAddressOption = asio::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>;
#else
    using ListenerAddressOption = asio::socket_base::reuse_address;
#endif

    asio::error_code ec;

    const bool dual_stack = address.is_v6() && address.is_unspecified();
    if (port != 0) {
        asio::ip::udp::socket udp_probe(group_.begin()->getExecutor());
        asio::ip::tcp::acceptor tcp_probe(group_.begin()->getExecutor());
        const asio::ip::udp::endpoint game{address, port};
        if (udp_probe.open(game.protocol(), ec) || (dual_stack && udp_probe.set_option(asio::ip::v6_only(false), ec)) ||
            udp_probe.bind(game, ec) || tcp_probe.open(asio::ip::tcp::endpoint{address, port}.protocol(), ec) ||
            tcp_probe.set_option(ListenerAddressOption(true), ec) ||
            (dual_stack && tcp_probe.set_option(asio::ip::v6_only(false), ec)) || tcp_probe.bind({address, port}, ec)) {
            logger_.error("Can't bind to {}:{}: {}", address.to_string(), port, ec.message());
            return false;
        }
    }
    std::vector<std::shared_ptr<asio::ip::udp::socket>> sockets;
    for (auto &loop : group_ | std::views::take(binds)) {
        auto &socket = sockets.emplace_back(std::make_shared<asio::ip::udp::socket>(loop.getExecutor()));
        const asio::ip::udp::endpoint game{address, port};
        if (socket->open(game.protocol(), ec) || (dual_stack && socket->set_option(asio::ip::v6_only(false), ec)) ||
#ifdef SO_REUSEPORT
            socket->set_option(ReusePort(true), ec) ||
#endif
            socket->bind(game, ec) || socket->non_blocking(true, ec)) {
            logger_.error("Can't bind to {}:{}: {}", address.to_string(), port, ec.message());
            return false;
        }
        port = socket->local_endpoint().port();
    }

    constexpr auto identity_key_path = "keys/server_identity_key.pem";

    const auto &configuration = server_.getConfiguration();
    std::shared_ptr<asio::ssl::context> tls;
    try {
        if (!identity_) {
            const auto key = nethernet::ServerIdentityAssertionGenerator::loadOrCreateKey(identity_key_path);
            identity_ = std::make_shared<const nethernet::ServerIdentityAssertionGenerator>(
                key, nethernet::ServerIdentityAssertionGenerator::Config{});
            offer_identity_ = std::make_shared<const nethernet::ServerIdentityAssertionGenerator>(
                key, nethernet::ServerIdentityAssertionGenerator::Config{
                         .cpk_encoding = nethernet::CpkEncoding::SubjectPublicKeyInfo});
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

    offline_.setPingResponder(port);
    auto socket = sockets.begin();
    for (auto &loop : group_ | std::views::take(sockets.size())) {
        endpoints_.emplace_back(server_, loop, registry_, *socket++, offline_.getGuid(),
                                offline_.getOfflinePingResponse(), identity_, offer_identity_, tls);
    }
    try {
        for (auto &endpoint : endpoints_) {
            endpoint.listen({address, port});
        }
    }
    catch (const std::system_error &e) {
        logger_.error("Can't bind to {}:{}: {}", address.to_string(), port, e.code().message());
        std::latch closed(static_cast<std::ptrdiff_t>(endpoints_.size()));
        for (auto &endpoint : endpoints_) {
            endpoint.getEventLoop().execute([&endpoint, &closed] {
                endpoint.close();
                closed.count_down();
            });
        }
        closed.wait();
        endpoints_.clear();
        return false;
    }

    logger_.info("Listening on {}", endpoints_.front().getLocalAddress());
    for (auto &endpoint : endpoints_) {
        endpoint.getEventLoop().execute([&endpoint] { endpoint.start(); });
    }

    game_port_ = port;
    if (configuration.isQueryEnabled()) {
        queryBind(address, static_cast<std::uint16_t>(configuration.getQueryPort()));
    }
    return true;
}

void ConnectionManager::queryBind(const asio::ip::address &address, std::uint16_t port)
{
    if (port == game_port_) {
        logger_.info("Listening for GS4 query on {}", endpoints_.front().getLocalAddress());
        for (auto &endpoint : endpoints_) {
            endpoint.getEventLoop().execute([&endpoint, this] { endpoint.setQueryHandler(&query_); });
        }
        return;
    }
    query_.queryBind(group_.next(), address, port);
}

void ConnectionManager::queryClose()
{
    for (auto &endpoint : endpoints_) {
        endpoint.getEventLoop().execute([&endpoint] { endpoint.setQueryHandler(nullptr); });
    }
    query_.close();
}

void ConnectionManager::start()
{
    group_.begin()->execute([this] {
        signals_.async_wait([this](const asio::error_code &ec, int) {
            if (!ec) {
                server_.shutdown(false);
            }
        });
    });
}

void ConnectionManager::shutdown()
{
    if (shut_down_.exchange(true)) {
        return;
    }
    for (auto &endpoint : endpoints_) {
        endpoint.getEventLoop().execute([&endpoint] { endpoint.close(); });
    }
    query_.close();
    group_.begin()->execute([this] { signals_.cancel(); });
    group_.shutdownGracefully();
}

void ConnectionManager::awaitTermination()
{
    group_.awaitTermination();
}

}  // namespace endgate
