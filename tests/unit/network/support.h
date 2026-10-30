#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "endgate/connection/connection.h"
#include "endgate/network/endpoint.h"
#include "endgate/network/eventloop.h"
#include "endgate/network/registry.h"
#include "endgate/network/rtc/certificate.h"
#include "endgate/network/rtc/description.h"
#include "endgate/network/rtc/ice.h"
#include "endgate/network/rtc/stun.h"
#include "endgate/server.h"

namespace endgate {

inline const asio::ip::address LOOPBACK = asio::ip::make_address("127.0.0.1");

class Node {
public:
    Node(ProxyServer &server, Registry &registry)
        : socket_(std::make_shared<asio::ip::udp::socket>(loop_.getExecutor(), asio::ip::udp::endpoint{LOOPBACK, 0})),
          endpoint_(server, loop_, registry, socket_, 1, std::span<const std::byte>{}, nullptr, nullptr, nullptr)
    {
        loop_.execute([this] { endpoint_.start(); });
    }

    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;
    Node(Node &&) = delete;
    Node &operator=(Node &&) = delete;

    ~Node()
    {
        loop_.execute([this] { endpoint_.close(); });
        loop_.shutdownGracefully();
        loop_.awaitTermination();
    }

    [[nodiscard]] Endpoint &endpoint() { return endpoint_; }
    [[nodiscard]] EventLoop &loop() { return loop_; }
    [[nodiscard]] asio::ip::udp::endpoint address() const { return {LOOPBACK, socket_->local_endpoint().port()}; }

    template <typename F>
    auto call(F &&query)
    {
        std::promise<decltype(query())> result;
        loop_.execute([&] { result.set_value(query()); });
        return result.get_future().get();
    }

    [[nodiscard]] std::size_t connections()
    {
        return call([this] { return endpoint_.getConnectionCount(); });
    }
    [[nodiscard]] std::size_t forwards()
    {
        return call([this] { return endpoint_.getForwardCount(); });
    }

    void close()
    {
        std::promise<void> closed;
        loop_.execute([&] {
            endpoint_.close();
            closed.set_value();
        });
        closed.get_future().wait();
    }

private:
    EventLoop loop_;
    std::shared_ptr<asio::ip::udp::socket> socket_;
    Endpoint endpoint_;
};

struct Join {
    rtc::IceCredentials local;
    rtc::IceCredentials remote;
    std::string username;
};

[[nodiscard]] inline PendingJoin makePending(const rtc::IceCredentials &local, const rtc::IceCredentials &remote)
{
    const std::array addresses{asio::ip::udp::endpoint{LOOPBACK, 41234}};
    return {.offer = rtc::Description::offer(remote, rtc::Certificate::shared()->getFingerprint(), addresses),
            .credentials = local,
            .network_id = "test",
            .created = std::chrono::steady_clock::now()};
}

[[nodiscard]] inline ByteBuf makeCheck(const Join &join, std::string_view key)
{
    static std::uint8_t transaction = 0;
    rtc::StunMessage request;
    request.transaction_id[0] = static_cast<std::byte>(++transaction);
    request.username = join.username;
    request.priority = 1845501695;
    request.ice_controlling = 0x1122334455667788ULL;
    return request.encode(key);
}

[[nodiscard]] inline Join makeJoin()
{
    Join join{.local = rtc::IceCredentials::generate(), .remote = rtc::IceCredentials::generate(), .username = {}};
    join.username = join.local.ufrag + ":" + join.remote.ufrag;
    return join;
}

/**
 * A connection with no transport under it: every batch it sends is kept for the test to read.
 */
class TestConnection final : public MinecraftConnection {
public:
    using MinecraftConnection::MinecraftConnection;

    void handleDatagram(std::span<const std::byte>, const asio::ip::udp::endpoint &) override {}

    [[nodiscard]] const std::vector<ByteBuf> &batches() const { return batches_; }
    [[nodiscard]] std::size_t sent() const { return sent_; }
    [[nodiscard]] bool closed() const { return closed_; }

protected:
    void send(ByteBuf batch) override
    {
        batches_.push_back(std::move(batch));
        ++sent_;
    }

    void disconnect(bool) override
    {
        closed_ = true;
        channelInactive();
    }

private:
    std::vector<ByteBuf> batches_;
    std::atomic<std::size_t> sent_{0};
    std::atomic<bool> closed_{false};
};

class Client {
public:
    Client() : socket_(io_, asio::ip::udp::endpoint{LOOPBACK, 0}) {}

    [[nodiscard]] asio::ip::udp::endpoint address() const { return {LOOPBACK, socket_.local_endpoint().port()}; }

    void send(const asio::ip::udp::endpoint &to, ByteBufView datagram)
    {
        socket_.send_to(asio::buffer(datagram.nioBuffer()), to);
    }

    [[nodiscard]] std::optional<std::pair<rtc::StunMessage, asio::ip::udp::endpoint>> receive(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{500})
    {
        std::array<std::byte, 2048> buffer{};
        asio::ip::udp::endpoint from;
        std::optional<std::size_t> size;
        io_.restart();
        socket_.async_receive_from(asio::buffer(buffer), from, [&](const asio::error_code &ec, std::size_t received) {
            if (!ec) {
                size = received;
            }
        });
        io_.run_for(timeout);
        if (!size) {
            socket_.cancel();
            io_.restart();
            io_.poll();
            return std::nullopt;
        }
        auto message = rtc::StunMessage::decode(Unpooled::wrappedBuffer(std::span{buffer}.first(*size)));
        if (!message) {
            return std::nullopt;
        }
        return std::pair{std::move(*message), from};
    }

private:
    asio::io_context io_;
    asio::ip::udp::socket socket_;
};

}  // namespace endgate
