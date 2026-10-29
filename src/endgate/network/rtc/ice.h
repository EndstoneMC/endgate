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

/**
 * A controlling, full ICE agent for one data stream of one component: the dialer's side. It gathers no
 * candidates of its own beyond the single socket the caller owns; it pairs that socket with every remote
 * candidate the answer carries, paces a connectivity check to each with ICE-CONTROLLING and the
 * tie-breaker, retransmits with an RTO backoff, nominates the first pair that answers with USE-CANDIDATE,
 * and keeps consent on the nominated pair. It also answers the checks the peer's full agent sends, so the
 * pair validates in both directions, and yields the controlling role on a 487.
 *
 * @see rtc::impl::IceTransport
 * @see RFC 8445 sections 6.1, 7.2, 8
 * @see RFC 7675
 */
class IceController {
public:
    enum class State {
        Checking,
        Connected,
        Failed,
    };

    enum class Role {
        Controlling,
        Controlled,
    };

    struct Config {
        std::chrono::milliseconds check_pacing{std::chrono::milliseconds{50}};
        std::chrono::milliseconds initial_rto{std::chrono::milliseconds{500}};
        std::chrono::milliseconds max_rto{std::chrono::seconds{3}};
        unsigned max_check_transmits{7};
        std::chrono::milliseconds negotiation_timeout{std::chrono::seconds{10}};
        std::chrono::milliseconds consent_interval{std::chrono::seconds{5}};
        std::chrono::milliseconds consent_timeout{std::chrono::seconds{20}};
        std::size_t max_candidate_pairs{16};
    };

    struct Statistics {
        std::uint64_t checks_sent{0};
        std::uint64_t responses_accepted{0};
        std::uint64_t checks_answered{0};
        std::uint64_t role_conflicts{0};
        std::uint64_t datagrams_dropped{0};
    };

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void sendIceDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) = 0;
        virtual void onSelectedPairChanged(const std::optional<asio::ip::udp::endpoint> &remote) = 0;
        virtual void onIceStateChanged(State state) = 0;
    };

    IceController(Listener &listener, IceCredentials local, std::string remote_ufrag, std::string remote_pwd,
                  std::span<const asio::ip::udp::endpoint> remote_candidates, std::uint64_t tie_breaker, Config config,
                  std::chrono::steady_clock::time_point now);

    [[nodiscard]] static std::uint64_t generateTieBreaker();

    [[nodiscard]] const IceCredentials &getLocalCredentials() const { return local_; }
    [[nodiscard]] State getState() const { return state_; }
    [[nodiscard]] Role getRole() const { return role_; }
    [[nodiscard]] const std::optional<asio::ip::udp::endpoint> &getSelectedPair() const { return selected_; }
    [[nodiscard]] const Statistics &getStatistics() const { return statistics_; }

    /**
     * @see RFC 8838 section 10
     */
    void addRemoteCandidate(std::chrono::steady_clock::time_point now, const asio::ip::udp::endpoint &remote);

    /**
     * @see RFC 8445 section 7.2.5
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
    enum class CheckState {
        Waiting,
        InProgress,
        Succeeded,
        Failed,
    };

    struct CandidatePair {
        asio::ip::udp::endpoint remote;
        std::uint32_t priority;
        CheckState state{CheckState::Waiting};
        bool nominating{false};
        bool use_candidate{false};
        unsigned transmits{0};
        StunMessage::TransactionId transaction_id{};
        std::chrono::steady_clock::time_point next_transmit;
        std::chrono::steady_clock::time_point last_response;
    };

    CandidatePair *findByTransaction(const StunMessage::TransactionId &transaction_id);
    CandidatePair *findByRemote(const asio::ip::udp::endpoint &remote);
    void sendCheck(std::chrono::steady_clock::time_point now, CandidatePair &pair, bool use_candidate);
    void respond(const asio::ip::udp::endpoint &remote, const StunMessage &request);
    void respondWithError(const asio::ip::udp::endpoint &remote, const StunMessage &request, StunErrorCode error,
                          std::vector<std::uint16_t> unknown_attributes);
    void onResponse(std::chrono::steady_clock::time_point now, CandidatePair &pair);
    void nominate(std::chrono::steady_clock::time_point now, CandidatePair &pair);
    void select(const asio::ip::udp::endpoint &remote);
    void setState(State state);

    Listener &listener_;
    IceCredentials local_;
    std::string remote_ufrag_;
    std::string remote_pwd_;
    std::uint64_t tie_breaker_;
    Config config_;
    std::chrono::steady_clock::time_point negotiation_deadline_;
    std::vector<CandidatePair> pairs_;
    std::optional<asio::ip::udp::endpoint> selected_;
    Role role_{Role::Controlling};
    State state_{State::Checking};
    bool nominated_{false};
    Statistics statistics_;
};

}  // namespace endgate::rtc
