#include "endgate/backend.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <random>
#include <span>
#include <utility>

#include <asio/as_tuple.hpp>
#include <asio/buffer.hpp>
#include <asio/cancel_after.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include "endgate/awaitable.h"
#include "endgate/logging/manager.h"
#include "endgate/network/nethernet/client.h"
#include "endgate/network/raknet/codec.h"
#include "endgate/network/raknet/message.h"
#include "endgate/network/raknet/protocol.h"

namespace endgate {

ServerInfo::ServerInfo(std::string name, std::string address, std::string host, std::uint16_t port,
                       std::optional<Transport> transport)
    : name_(std::move(name)), address_(std::move(address)), host_(std::move(host)), port_(port), transport_(transport)
{
}

std::optional<ServerInfo> ServerInfo::parse(std::string name, std::string_view address)
{
    constexpr std::uint16_t default_port = 19132;
    constexpr std::string_view nethernet_scheme = "nethernet://";
    constexpr std::string_view raknet_scheme = "raknet://";

    auto authority = address;
    std::optional<Transport> transport;
    if (authority.starts_with(nethernet_scheme)) {
        transport = Transport::NetherNet;
        authority.remove_prefix(nethernet_scheme.size());
    }
    else if (authority.starts_with(raknet_scheme)) {
        transport = Transport::RakNet;
        authority.remove_prefix(raknet_scheme.size());
    }
    else if (authority.contains("://")) {
        return std::nullopt;
    }

    std::string_view host = authority;
    std::optional<std::string_view> port;
    if (authority.starts_with('[')) {
        const auto bracket = authority.find(']');
        if (bracket == std::string_view::npos) {
            return std::nullopt;
        }
        host = authority.substr(1, bracket - 1);
        const auto rest = authority.substr(bracket + 1);
        if (!rest.empty() && !rest.starts_with(':')) {
            return std::nullopt;
        }
        if (!rest.empty()) {
            port = rest.substr(1);
        }
    }
    else if (const auto colon = authority.find(':'); colon != std::string_view::npos && colon == authority.rfind(':')) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }

    std::uint16_t number = default_port;
    if (port) {
        const auto [end, error] = std::from_chars(port->data(), port->data() + port->size(), number);
        if (error != std::errc{} || end != port->data() + port->size() || number == 0) {
            return std::nullopt;
        }
    }
    if (host.empty()) {
        return std::nullopt;
    }
    return ServerInfo{std::move(name), std::string(address), std::string(host), number, transport};
}

BackendServer::BackendServer(ServerInfo info) : info_(std::move(info)), logger_(LogManager::getLogger(*this)) {}

Awaitable<std::optional<ServerInfo::Transport>> BackendServer::detectTransport(asio::io_context &io_context,
                                                                               asio::ip::udp::endpoint address)
{
    constexpr std::chrono::milliseconds signaling_timeout{2000};
    constexpr std::chrono::milliseconds ping_timeout{1000};

    if (const auto pinned = info_.getTransport()) {
        co_return pinned;
    }
    std::optional<ServerInfo::Transport> detected;
    {
        const std::scoped_lock lock(mutex_);
        detected = detected_;
    }
    if (detected) {
        co_return detected;
    }

    const auto probe =
        co_await nethernet::SignalingClient::probeNetherNet(io_context, info_.getHost(), info_.getPort(),
                                                            {.connect_timeout = signaling_timeout,
                                                             .request_timeout = signaling_timeout,
                                                             .tls_handshake_timeout = signaling_timeout});
    if (probe.supported) {
        detected = ServerInfo::Transport::NetherNet;
    }
    else if (co_await ping(address, ping_timeout)) {
        detected = ServerInfo::Transport::RakNet;
    }
    if (!detected) {
        logger_.debug("Server {} supports neither NetherNet nor RakNet", info_.getName());
        co_return detected;
    }
    logger_.debug("Server {} uses {}", info_.getName(), toString(*detected));
    const std::scoped_lock lock(mutex_);
    detected_ = detected;
    co_return detected;
}

void BackendServer::forgetTransport()
{
    const std::scoped_lock lock(mutex_);
    detected_.reset();
}

Awaitable<bool> BackendServer::ping(asio::ip::udp::endpoint address, std::chrono::milliseconds timeout)
{
    constexpr int attempts = 4;

    asio::ip::udp::socket socket(co_await asio::this_coro::executor);
    asio::error_code ec;
    if (socket.open(address.protocol(), ec) || socket.connect(address, ec)) {
        co_return false;
    }

    const auto started = std::chrono::steady_clock::now();
    auto request = Unpooled::buffer(64, 64);
    raknet::UnconnectedPing{
        .id = raknet::MessageId::UnconnectedPing,
        .send_ping_time = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(started.time_since_epoch()).count()),
        .client_guid = std::mt19937_64{std::random_device{}()}()}
        .encode(request);

    std::array<std::byte, raknet::MAXIMUM_MTU_SIZE> response{};
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        socket.send(asio::buffer(request.nioBuffer()), 0, ec);
        if (ec) {
            co_return false;
        }
        const auto deadline = started + timeout * attempt / attempts;
        for (auto now = std::chrono::steady_clock::now(); now < deadline; now = std::chrono::steady_clock::now()) {
            const auto [error, size] = co_await socket.async_receive(
                asio::buffer(response), asio::cancel_after(deadline - now, asio::as_tuple(asio::use_awaitable)));
            if (error == asio::error::operation_aborted) {
                break;
            }
            if (error) {
                co_return false;
            }
            if (raknet::UnconnectedPong::decode(Unpooled::wrappedBuffer(std::span{response}.first(size)))) {
                co_return true;
            }
        }
    }
    co_return false;
}

}  // namespace endgate
