#pragma once

#include <string>
#include <string_view>
#include <variant>

#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

namespace endgate {

/**
 * The kind of traffic a listener endpoint serves.
 *
 * @see RakPeerHelper::PeerPurpose
 * @see com.velocitypowered.api.network.ListenerType
 */
enum class ListenerType {
    Gameplay,
    HttpSignaling,
    LanSignaling,
};

[[nodiscard]] constexpr std::string_view toString(ListenerType type)
{
    switch (type) {
    case ListenerType::Gameplay:
        return "Minecraft";
    case ListenerType::HttpSignaling:
        return "NetherNet signaling";
    case ListenerType::LanSignaling:
        return "NetherNet LAN signaling";
    }
    return "Unknown";
}

/**
 * Represents a listener endpoint.
 *
 * @see com.velocitypowered.proxy.network.Endpoint
 */
class Endpoint {
public:
    Endpoint(asio::ip::tcp::acceptor acceptor, ListenerType type) : channel_(std::move(acceptor)), type_(type) {}

    Endpoint(asio::ip::udp::socket socket, ListenerType type) : channel_(std::move(socket)), type_(type) {}

    [[nodiscard]] ListenerType getType() const { return type_; }

    [[nodiscard]] std::string getLocalAddress() const
    {
        return std::visit(
            [](const auto &channel) {
                const auto local = channel.local_endpoint();
                return local.address().to_string() + ":" + std::to_string(local.port());
            },
            channel_);
    }

    [[nodiscard]] asio::ip::tcp::acceptor &getAcceptor() { return std::get<asio::ip::tcp::acceptor>(channel_); }

    [[nodiscard]] asio::ip::udp::socket &getSocket() { return std::get<asio::ip::udp::socket>(channel_); }

    void close()
    {
        std::visit(
            [](auto &channel) {
                asio::error_code ignored;
                channel.close(ignored);
            },
            channel_);
    }

private:
    std::variant<asio::ip::tcp::acceptor, asio::ip::udp::socket> channel_;
    ListenerType type_;
};

}  // namespace endgate
