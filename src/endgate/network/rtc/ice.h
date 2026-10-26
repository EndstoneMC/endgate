#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <asio/ip/udp.hpp>

#include "endgate/network/rtc/stun.h"

namespace endgate::rtc {

/**
 * @see RFC 8839 section 5.4
 */
struct IceCredentials {
    std::string ufrag;
    std::string pwd;

    [[nodiscard]] static IceCredentials generate();
};

/**
 * An ICE-lite agent for one data stream of one component. It answers the checks a full agent sends,
 * keeps a candidate pair for every address a valid check arrives from, and sends on the pair the full
 * agent nominated last. It never sends a check of its own.
 *
 * @see rtc::impl::IceTransport
 * @see RFC 8445 section 8.2
 * @see RFC 7675
 */
class IceTransport {
public:
    enum class State {
        Checking,
        Connected,
        Disconnected,
        Failed,
    };

    struct Config {
        std::chrono::milliseconds negotiation_timeout{std::chrono::seconds{10}};
        std::chrono::milliseconds consent_timeout{std::chrono::seconds{30}};
        std::size_t max_candidate_pairs{8};
    };

    struct Statistics {
        std::uint64_t checks_answered{0};
        std::uint64_t datagrams_dropped{0};
    };

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void sendIceDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) = 0;
        virtual void onCandidatePairAdded(const asio::ip::udp::endpoint &remote) = 0;
        virtual void onCandidatePairRemoved(const asio::ip::udp::endpoint &remote) = 0;
        virtual void onSelectedPairChanged(const std::optional<asio::ip::udp::endpoint> &remote) = 0;
        virtual void onIceStateChanged(State state) = 0;
    };

    IceTransport(Listener &listener, IceCredentials local, std::string remote_ufrag, Config config,
                 std::chrono::steady_clock::time_point now);

    [[nodiscard]] const IceCredentials &getLocalCredentials() const { return local_; }
    [[nodiscard]] State getState() const { return state_; }
    [[nodiscard]] const std::optional<asio::ip::udp::endpoint> &getSelectedPair() const { return selected_; }
    [[nodiscard]] const Statistics &getStatistics() const { return statistics_; }

    /**
     * @see RFC 8445 section 7.3
     */
    void handleDatagram(std::chrono::steady_clock::time_point now, const asio::ip::udp::endpoint &remote,
                        std::span<const std::byte> datagram);

    /**
     * @see RFC 8445 section 12.1.1
     */
    bool send(std::span<const std::byte> datagram);

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> nextTimeout() const;
    void handleTimeout(std::chrono::steady_clock::time_point now);

private:
    struct CandidatePair {
        asio::ip::udp::endpoint remote;
        std::chrono::steady_clock::time_point last_check;
        std::optional<std::chrono::steady_clock::time_point> nominated_at;
        std::uint32_t priority;
    };

    void respondWithError(const asio::ip::udp::endpoint &remote, const StunMessage &request, StunErrorCode error,
                          std::vector<std::uint16_t> unknown_attributes);
    void setState(State state);
    void reselect();

    Listener &listener_;
    IceCredentials local_;
    std::string remote_ufrag_;
    Config config_;
    std::chrono::steady_clock::time_point negotiation_deadline_;
    std::vector<CandidatePair> pairs_;
    std::optional<asio::ip::udp::endpoint> selected_;
    State state_{State::Checking};
    Statistics statistics_;
};

}  // namespace endgate::rtc
