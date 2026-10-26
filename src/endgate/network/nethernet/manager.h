#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <asio/ip/udp.hpp>

#include "endgate/network/buffer.h"
#include "endgate/network/nethernet/identity.h"
#include "endgate/network/nethernet/session.h"
#include "endgate/network/nethernet/signaling.h"
#include "endgate/network/rtc/certificate.h"
#include "endgate/network/rtc/description.h"

namespace endgate::nethernet {

/**
 * Every NetherNet session behind one socket. It answers joins, hands each datagram to the session it
 * belongs to, by the address ICE paired or by the ufrag of a first check, and keeps the sessions'
 * timers.
 *
 * @see NetherNet::NetworkSessionManager
 */
class NetworkSessionManager {
public:
    using Clock = std::chrono::steady_clock;
    using SessionId = std::uint64_t;

    struct Config {
        std::shared_ptr<const rtc::Certificate> certificate;
        std::shared_ptr<const ServerIdentityAssertionGenerator> identity;
        NetworkSession::Config session;
        bool require_identity{false};
    };

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void sendDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) = 0;
        virtual void onSessionOpen(SessionId session, const std::string &network_id) = 0;
        virtual void onPacket(SessionId session, ByteBuf packet) = 0;
        virtual void onSessionClosed(SessionId session, ESessionError reason) = 0;
    };

    NetworkSessionManager(Listener &listener, Config config);

    /**
     * @see NetherNet::NetworkSessionManager::_onConnectRequest
     */
    [[nodiscard]] std::expected<std::string, ESessionError> join(const JoinRequest &request,
                                                                 std::span<const rtc::CandidateAddress> candidates,
                                                                 Clock::time_point now,
                                                                 std::chrono::system_clock::time_point wall_clock);

    bool handleDatagram(Clock::time_point now, const asio::ip::udp::endpoint &remote,
                        std::span<const std::byte> datagram);

    bool send(SessionId session, const ByteBuf &packet, ESendType type, Clock::time_point now);
    void close(SessionId session, Clock::time_point now);

    [[nodiscard]] std::optional<Clock::time_point> nextTimeout() const;
    void handleTimeout(Clock::time_point now);

    [[nodiscard]] std::size_t size() const { return sessions_.size(); }

private:
    class Entry : public NetworkSession::Listener {
    public:
        Entry(NetworkSessionManager &manager, SessionId id, std::string network_id, std::string ufrag,
              const rtc::Description &offer, rtc::IceCredentials local, Clock::time_point now);

        void sendDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) override;
        void onRouteAdded(const asio::ip::udp::endpoint &remote) override;
        void onRouteRemoved(const asio::ip::udp::endpoint &remote) override;
        void onOpen() override;
        void onPacket(ByteBuf packet) override;
        void onClosed(ESessionError reason) override;

        NetworkSessionManager &manager;
        SessionId id;
        std::string network_id;
        std::string ufrag;
        std::optional<ESessionError> closed;
        NetworkSession session;
    };

    void reap();

    Listener &listener_;
    Config config_;
    SessionId next_id_{1};
    std::map<SessionId, std::unique_ptr<Entry>> sessions_;
    std::unordered_map<std::string, SessionId> ufrags_;
    std::map<asio::ip::udp::endpoint, SessionId> routes_;
    unsigned depth_{0};
};

}  // namespace endgate::nethernet
