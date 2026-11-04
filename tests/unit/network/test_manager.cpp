#include <filesystem>
#include <random>
#include <string>
#include <system_error>

#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <gtest/gtest.h>

#include "endgate/network/manager.h"
#include "endgate/server.h"

namespace endgate {
namespace {

class WorkingDirectory : public testing::Environment {
public:
    void SetUp() override
    {
        directory_ =
            std::filesystem::temp_directory_path() / ("endgate-tests-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(directory_);
        std::filesystem::current_path(directory_);
    }

    void TearDown() override
    {
        std::filesystem::current_path(std::filesystem::temp_directory_path());
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

private:
    std::filesystem::path directory_;
};

const auto *const WORKING_DIRECTORY = testing::AddGlobalTestEnvironment(new WorkingDirectory);

const auto LOOPBACK = asio::ip::make_address("127.0.0.1");

[[nodiscard]] std::uint16_t freePort()
{
    asio::io_context io;
    for (;;) {
        asio::ip::udp::socket udp(io, asio::ip::udp::endpoint{LOOPBACK, 0});
        const auto port = udp.local_endpoint().port();
        asio::ip::tcp::acceptor tcp(io);
        asio::error_code ec;
        if (!tcp.open(asio::ip::tcp::v4(), ec) && !tcp.bind({LOOPBACK, port}, ec)) {
            return port;
        }
    }
}

TEST(ConnectionManagerTest, BindsTheGroupOnOnePort)
{
    ProxyServer server;
    ConnectionManager manager(server);
    EXPECT_TRUE(manager.bind(LOOPBACK, freePort()));
}

TEST(ConnectionManagerTest, ASecondInstanceCannotBindTheSamePort)
{
    const auto port = freePort();
    ProxyServer first_server;
    ProxyServer second_server;
    ConnectionManager first(first_server);
    ConnectionManager second(second_server);
    ASSERT_TRUE(first.bind(LOOPBACK, port));
    EXPECT_FALSE(second.bind(LOOPBACK, port));
}

TEST(ConnectionManagerTest, ATakenUdpPortFailsTheBind)
{
    asio::io_context io;
    asio::ip::udp::socket taken(io, asio::ip::udp::endpoint{LOOPBACK, 0});
    ProxyServer server;
    ConnectionManager manager(server);
    EXPECT_FALSE(manager.bind(LOOPBACK, taken.local_endpoint().port()));
}

TEST(ConnectionManagerTest, ATakenTcpPortFailsTheBind)
{
    asio::io_context io;
    asio::ip::tcp::acceptor taken(io, asio::ip::tcp::endpoint{LOOPBACK, 0});
    ProxyServer server;
    ConnectionManager manager(server);
    EXPECT_FALSE(manager.bind(LOOPBACK, taken.local_endpoint().port()));
}

TEST(ConnectionManagerTest, ShutsDownAfterAFailedBind)
{
    asio::io_context io;
    asio::ip::tcp::acceptor taken(io, asio::ip::tcp::endpoint{LOOPBACK, 0});
    ProxyServer server;
    {
        ConnectionManager manager(server);
        ASSERT_FALSE(manager.bind(LOOPBACK, taken.local_endpoint().port()));
        manager.shutdown();
        manager.awaitTermination();
    }
    SUCCEED();
}

}  // namespace
}  // namespace endgate
