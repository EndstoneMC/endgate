#pragma once

#include <string>

#include <asio/ip/udp.hpp>

namespace endgate {

/**
 * Represents a listener endpoint.
 *
 * @see com.velocitypowered.proxy.network.Endpoint
 */
class Endpoint {
public:
    explicit Endpoint(asio::ip::udp::socket socket) : socket_(std::move(socket)) {}

    [[nodiscard]] std::string getLocalAddress() const
    {
        const auto local = socket_.local_endpoint();
        return local.address().to_string() + ":" + std::to_string(local.port());
    }

    [[nodiscard]] asio::ip::udp::socket &getSocket() { return socket_; }

    void close()
    {
        asio::error_code ignored;
        socket_.close(ignored);
    }

private:
    asio::ip::udp::socket socket_;
};

}  // namespace endgate
