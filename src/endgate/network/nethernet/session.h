#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <asio/ip/udp.hpp>

#include "endgate/network/buffer.h"
#include "endgate/network/nethernet/framing.h"
#include "endgate/network/rtc/certificate.h"
#include "endgate/network/rtc/description.h"
#include "endgate/network/rtc/dtls.h"
#include "endgate/network/rtc/ice.h"
#include "endgate/network/rtc/sctp/transport.h"

namespace endgate::nethernet {

/**
 * @see NetherNet::ESessionError
 */
enum class ESessionError : std::uint32_t {
    None = 0,
    DestinationNotLoggedIn = 1,
    NegotiationTimeout = 2,
    WrongTransportVersion = 3,
    FailedToCreatePeerConnection = 4,
    ICE = 5,
    ConnectRequest = 6,
    ConnectResponse = 7,
    CandidateAdd = 8,
    InactivityTimeout = 9,
    FailedToCreateOffer = 10,
    FailedToCreateAnswer = 11,
    FailedToSetLocalDescription = 12,
    FailedToSetRemoteDescription = 13,
    NegotiationTimeoutWaitingForResponse = 14,
    NegotiationTimeoutWaitingForAccept = 15,
    IncomingConnectionIgnored = 16,
    SignalingParsingFailure = 17,
    SignalingUnknownError = 18,
    SignalingUnicastMessageDeliveryFailed = 19,
    SignalingBroadcastDeliveryFailed = 20,
    SignalingMessageDeliveryFailed = 21,
    SignalingTurnAuthFailed = 22,
    SignalingFallbackToBestEffortDelivery = 23,
    NoSignalingChannel = 24,
    NotLoggedIn = 25,
    SignalingFailedToSend = 26,
    RelayServerConfigurationResultFailure = 27,
    RelayServerConfigurationResultParsingErrorNoUrls = 28,
    RelayServerConfigurationResultParsingErrorNoCreds = 29,
    RelayServerConfigurationResultParsingErrorNoServers = 30,
    RelayServerConfigurationResultParsingErrorNoExpiration = 31,
    DataChannelClosed = 32,
    InternalErrorJsonSerialization = 33,
    InvalidArgument = 34,
    GenericFailure = 35,
    FailedToCreateIdentityAssertion = 36,
    IdentityNotAllowed = 37,
};

/**
 * One client's NetherNet session: ICE-lite, DTLS and SCTP stacked under the two data channels the
 * client opens, with NetherNet's fragment framing on top.
 *
 * @see NetherNet::NetworkSession
 */
class NetworkSession : rtc::IceTransport::Listener,
                       rtc::DtlsTransport::Listener,
                       rtc::SctpTransport::Listener,
                       PacketQueue::Listener {
public:
    using Clock = std::chrono::steady_clock;

    struct Config {
        std::size_t max_packet_size;
        rtc::IceTransport::Config ice{};
        rtc::DtlsTransport::Config dtls{};
        rtc::sctp::Association::Config association{};
    };

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void sendDatagram(const asio::ip::udp::endpoint &remote, std::span<const std::byte> datagram) = 0;
        virtual void onRouteAdded(const asio::ip::udp::endpoint &remote) = 0;
        virtual void onRouteRemoved(const asio::ip::udp::endpoint &remote) = 0;
        virtual void onOpen() = 0;
        virtual void onPacket(LegacyByteBuf packet) = 0;
        virtual void onClosed(ESessionError reason) = 0;
    };

    NetworkSession(Listener &listener, const rtc::Description &offer, rtc::IceCredentials local,
                   std::shared_ptr<const rtc::Certificate> certificate, const Config &config, Clock::time_point now);
    NetworkSession(const NetworkSession &) = delete;
    NetworkSession &operator=(const NetworkSession &) = delete;
    NetworkSession(NetworkSession &&) = delete;
    NetworkSession &operator=(NetworkSession &&) = delete;
    ~NetworkSession() override = default;

    void handleDatagram(Clock::time_point now, const asio::ip::udp::endpoint &remote,
                        std::span<const std::byte> datagram);

    /**
     * @see NetherNet::NetworkSession::sendPacket
     */
    bool send(const LegacyByteBuf &packet, ESendType type, Clock::time_point now);

    void close(Clock::time_point now);

    [[nodiscard]] std::optional<Clock::time_point> nextTimeout() const;
    void handleTimeout(Clock::time_point now);

    [[nodiscard]] bool isOpen() const { return open_ && !closed_; }
    [[nodiscard]] bool isClosed() const { return closed_.has_value(); }
    [[nodiscard]] const rtc::IceTransport &getIceTransport() const { return ice_; }

private:
    void sendIceDatagram(const asio::ip::udp::endpoint &remote, ByteBufView datagram) override;
    void onCandidatePairAdded(const asio::ip::udp::endpoint &remote) override;
    void onCandidatePairRemoved(const asio::ip::udp::endpoint &remote) override;
    void onSelectedPairChanged(const std::optional<asio::ip::udp::endpoint> &remote) override;
    void onIceStateChanged(rtc::IceTransport::State state) override;

    void sendDtlsDatagram(ByteBufView datagram) override;
    void onDtlsData(ByteBufView data) override;
    void onDtlsStateChanged(rtc::DtlsTransport::State state) override;

    void sendPacket(ByteBufView packet) override;
    void onStateChange(rtc::SctpTransport::State state) override;
    void onChannelOpen(const rtc::DataChannel &channel) override;
    void onChannelMessage(const rtc::DataChannel &channel, ByteBufView message, bool binary) override;
    void onChannelClosed(const rtc::DataChannel &channel) override;

    void sendFragment(ESendType type, LegacyByteBuf fragment) override;
    void onPacket(LegacyByteBuf packet) override;
    void closeChannel(ESendType type) override;

    void fail(ESessionError reason);
    void advance();

    Listener &listener_;
    Clock::time_point now_;
    rtc::IceTransport ice_;
    rtc::DtlsTransport dtls_;
    rtc::SctpTransport sctp_;
    PacketQueue queue_;
    std::vector<asio::ip::udp::endpoint> pairs_;
    std::vector<std::vector<std::byte>> outbox_;
    std::optional<std::uint16_t> reliable_stream_;
    std::optional<std::uint16_t> unreliable_stream_;
    unsigned depth_{0};
    bool sctp_started_{false};
    bool open_{false};
    std::optional<ESessionError> closed_;
    bool close_reported_{false};
};

/**
 * The dialer's NetherNet session: a controlling full ICE agent, DTLS in the role the answer assigns,
 * and SCTP over which this side opens the two data channels a Bedrock client opens — ReliableDataChannel
 * ordered and reliable, UnreliableDataChannel unordered with maxRetransmits 0 — with NetherNet's fragment
 * framing on top. The reported events and ESessionError reporting match the answering side.
 *
 * @see NetherNet::NetworkSession
 */
class DialerSession : rtc::IceController::Listener,
                      rtc::DtlsTransport::Listener,
                      rtc::SctpTransport::Listener,
                      PacketQueue::Listener {
public:
    using Clock = std::chrono::steady_clock;
    using Listener = NetworkSession::Listener;

    struct Config {
        std::size_t max_packet_size;
        rtc::IceController::Config ice{};
        rtc::DtlsTransport::Config dtls{};
        rtc::sctp::Association::Config association{};
    };

    DialerSession(Listener &listener, const rtc::Description &answer, rtc::IceCredentials local,
                  std::shared_ptr<const rtc::Certificate> certificate, const Config &config, Clock::time_point now);
    DialerSession(const DialerSession &) = delete;
    DialerSession &operator=(const DialerSession &) = delete;
    DialerSession(DialerSession &&) = delete;
    DialerSession &operator=(DialerSession &&) = delete;
    ~DialerSession() override = default;

    void handleDatagram(Clock::time_point now, const asio::ip::udp::endpoint &remote,
                        std::span<const std::byte> datagram);

    /**
     * @see NetherNet::NetworkSession::sendPacket
     */
    bool send(const LegacyByteBuf &packet, ESendType type, Clock::time_point now);

    void close(Clock::time_point now);

    [[nodiscard]] std::optional<Clock::time_point> nextTimeout() const;
    void handleTimeout(Clock::time_point now);

    [[nodiscard]] bool isOpen() const { return open_ && !closed_; }
    [[nodiscard]] bool isClosed() const { return closed_.has_value(); }
    [[nodiscard]] const rtc::IceController &getIceController() const { return ice_; }
    [[nodiscard]] const std::vector<asio::ip::udp::endpoint> &getRemoteCandidates() const { return candidates_; }

private:
    [[nodiscard]] static std::vector<asio::ip::udp::endpoint> endpointsOf(const rtc::Description &answer);

    void sendIceDatagram(const asio::ip::udp::endpoint &remote, ByteBufView datagram) override;
    void onSelectedPairChanged(const std::optional<asio::ip::udp::endpoint> &remote) override;
    void onIceStateChanged(rtc::IceController::State state) override;

    void sendDtlsDatagram(ByteBufView datagram) override;
    void onDtlsData(ByteBufView data) override;
    void onDtlsStateChanged(rtc::DtlsTransport::State state) override;

    void sendPacket(ByteBufView packet) override;
    void onStateChange(rtc::SctpTransport::State state) override;
    void onChannelOpen(const rtc::DataChannel &channel) override;
    void onChannelMessage(const rtc::DataChannel &channel, ByteBufView message, bool binary) override;
    void onChannelClosed(const rtc::DataChannel &channel) override;

    void sendFragment(ESendType type, LegacyByteBuf fragment) override;
    void onPacket(LegacyByteBuf packet) override;
    void closeChannel(ESendType type) override;

    void fail(ESessionError reason);
    void advance();

    Listener &listener_;
    Clock::time_point now_;
    std::vector<asio::ip::udp::endpoint> candidates_;
    rtc::IceController ice_;
    rtc::DtlsTransport dtls_;
    rtc::SctpTransport sctp_;
    PacketQueue queue_;
    std::vector<std::vector<std::byte>> outbox_;
    std::optional<std::uint16_t> reliable_stream_;
    std::optional<std::uint16_t> unreliable_stream_;
    unsigned depth_{0};
    bool sctp_started_{false};
    bool channels_opened_{false};
    bool open_{false};
    std::optional<ESessionError> closed_;
    bool close_reported_{false};
};

}  // namespace endgate::nethernet
