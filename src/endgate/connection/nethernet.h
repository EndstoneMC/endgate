#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <spdlog/logger.h>

#include "endgate/connection/connection.h"
#include "endgate/network/nethernet/identity.h"
#include "endgate/network/nethernet/session.h"
#include "endgate/network/nethernet/signaling.h"
#include "endgate/network/rtc/certificate.h"
#include "endgate/network/rtc/description.h"
#include "endgate/network/rtc/ice.h"

namespace endgate {

/**
 * A player carried over NetherNet, answered on an endpoint's socket. NetherNet carries a batch as it
 * is, so the leg passes batches through unchanged.
 *
 * @see NetherNet::NetworkSession
 */
class NetherNetConnection final : public MinecraftConnection, private nethernet::NetworkSession::Listener {
public:
    NetherNetConnection(Endpoint &endpoint, const std::string &network_id, const rtc::Description &offer,
                        rtc::IceCredentials local, std::shared_ptr<const rtc::Certificate> certificate,
                        const nethernet::NetworkSession::Config &config, Clock::time_point now);

    /**
     * Answers the offer the connection was made from, signed with the server's identity.
     *
     * @see NetherNet::ServerNegotiator::createAnswer
     */
    void answer(rtc::Description offer, std::vector<rtc::CandidateAddress> candidates, std::uint64_t session_id,
                std::shared_ptr<const nethernet::ServerIdentityAssertionGenerator> identity,
                nethernet::JoinResponse response);

protected:
    void handleDatagram(std::span<const std::byte> datagram, const asio::ip::udp::endpoint &sender) override;
    void send(ByteBuf batch) override;
    void disconnect(bool wait_for_peer) override;

private:
    void sendDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) override;
    void onRouteAdded(const asio::ip::udp::endpoint &remote) override;
    void onRouteRemoved(const asio::ip::udp::endpoint &remote) override;
    void onOpen() override;
    void onPacket(ByteBuf packet) override;
    void onClosed(nethernet::ESessionError reason) override;

    void arm();

    spdlog::logger &logger_;
    std::shared_ptr<asio::ip::udp::socket> socket_;
    std::shared_ptr<const rtc::Certificate> certificate_;
    asio::steady_timer timer_;
    std::optional<Clock::time_point> armed_;
    bool active_{false};
    nethernet::NetworkSession session_;
};

}  // namespace endgate
