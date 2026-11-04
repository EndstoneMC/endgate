#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tuple>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include "endgate/awaitable.h"
#include "endgate/backend.h"
#include "endgate/buffer/bytebuf.h"
#include "endgate/network/raknet/message.h"
#include "endgate/network/raknet/protocol.h"
#include "endgate/network/version.h"

namespace endgate {
namespace {

std::string announcement(int protocol)
{
    return "MCPE;Dedicated Server;" + std::to_string(protocol) +
           ";1.26.60;0;10;1;Bedrock level;Survival;1;19132;19133;0;";
}

Awaitable<void> answerPings(asio::ip::udp::socket &socket, std::string motd)
{
    std::array<std::byte, raknet::MAXIMUM_MTU_SIZE> request{};
    asio::ip::udp::endpoint sender;
    while (true) {
        co_await socket.async_receive_from(asio::buffer(request), sender, asio::use_awaitable);
        auto data = Unpooled::buffer(motd.size() + 2, motd.size() + 2);
        data.writeShort(static_cast<std::uint16_t>(motd.size()));
        data.writeBytes(std::as_bytes(std::span{motd}));
        auto reply = Unpooled::buffer(128, 128);
        raknet::UnconnectedPong{.send_ping_time = 1, .server_guid = 2, .data = data.slice()}.encode(reply);
        co_await socket.async_send_to(asio::buffer(reply.nioBuffer()), sender, asio::use_awaitable);
    }
}

std::optional<int> protocolAfterDetecting(BackendServer &backend, int announced)
{
    asio::io_context io;
    asio::ip::udp::socket socket(io, asio::ip::udp::endpoint{asio::ip::make_address("127.0.0.1"), 0});
    const auto address = socket.local_endpoint();
    asio::co_spawn(io, answerPings(socket, announcement(announced)), asio::detached);
    auto done = asio::co_spawn(
        io,
        [&]() -> Awaitable<std::optional<int>> {
            std::ignore = co_await backend.detectTransport(io, address);
            io.stop();
            co_return backend.getProtocol();
        },
        asio::use_future);
    io.run();
    return done.get();
}

TEST(Routing, AProtocolIsNamedByItsVersionOrByItsNumber)
{
    EXPECT_EQ(describeProtocol(2193), "1.26.50");
    EXPECT_EQ(describeProtocol(1), "protocol 1");
}

TEST(Routing, ABackendWhoseProtocolIsUnknownIsOnNoOtherProtocol)
{
    BackendServer backend{ServerInfo::parse("lobby", "127.0.0.1:19132").value()};

    EXPECT_FALSE(backend.isOnOtherProtocol(2225));
}

TEST(Routing, ABackendIsOnOtherProtocolsThanTheOneItAnnounced)
{
    BackendServer backend{ServerInfo::parse("lobby", "raknet://127.0.0.1:19132").value()};
    EXPECT_EQ(protocolAfterDetecting(backend, 2193), 2193);

    EXPECT_TRUE(backend.isOnOtherProtocol(2225));
    EXPECT_FALSE(backend.isOnOtherProtocol(2193));
}

TEST(Routing, AForgottenBackendForgetsItsProtocolToo)
{
    BackendServer backend{ServerInfo::parse("lobby", "raknet://127.0.0.1:19132").value()};
    EXPECT_EQ(protocolAfterDetecting(backend, 2193), 2193);

    backend.forgetTransport();

    EXPECT_FALSE(backend.getProtocol().has_value());
    EXPECT_FALSE(backend.isOnOtherProtocol(2225));
}

}  // namespace
}  // namespace endgate
