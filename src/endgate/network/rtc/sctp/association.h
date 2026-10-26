#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "endgate/network/buffer.h"
#include "endgate/network/rtc/sctp/cookie.h"
#include "endgate/network/rtc/sctp/packet.h"
#include "endgate/network/rtc/sctp/receiver.h"
#include "endgate/network/rtc/sctp/sender.h"

namespace endgate::rtc::sctp {

/**
 * One SCTP association over DTLS, as a sans-IO state machine. Packets come in through handlePacket and leave through
 * the listener; time comes in with every call.
 *
 * Both sides of a WebRTC association send INIT once DTLS is up, so the collision of RFC 9260 section 5.2 is the normal
 * path. The association answers an INIT in every state and resolves crossing COOKIE ECHOs by section 5.2.4, whether
 * or not connect() was called.
 *
 * Listener callbacks other than sendPacket are queued and run once the call that caused them has finished, so a
 * listener may call back into the association. sendPacket must not, and no callback may destroy the association.
 *
 * @see RFC 9260 section 4
 * @see RFC 8261
 * @see dcsctp::DcSctpSocket
 */
class Association {
public:
    using Clock = std::chrono::steady_clock;

    enum class State {
        Closed,
        CookieWait,
        CookieEchoed,
        Established,
        ShutdownPending,
        ShutdownSent,
        ShutdownReceived,
        ShutdownAckSent,
    };

    enum class CloseReason {
        Shutdown,
        Aborted,
        PeerAborted,
        Timeout,
        ProtocolViolation,
        OutOfResource,
    };

    struct Config {
        std::uint16_t local_port{5000};
        std::uint16_t remote_port{5000};
        std::size_t mtu{1191};
        std::uint16_t streams{1024};
        std::size_t receive_window{1024 * 1024};
        std::size_t max_message_size{262144};
        std::size_t max_send_buffer{4 * 1024 * 1024};
        Clock::duration rto_initial{std::chrono::milliseconds{500}};
        Clock::duration rto_min{std::chrono::milliseconds{400}};
        Clock::duration rto_max{std::chrono::seconds{60}};
        Clock::duration init_timeout{std::chrono::seconds{1}};
        Clock::duration delayed_ack_timeout{std::chrono::milliseconds{200}};
        Clock::duration heartbeat_interval{std::chrono::seconds{30}};
        Clock::duration cookie_lifetime{std::chrono::seconds{60}};
        unsigned max_init_retransmits{8};
        unsigned max_retransmissions{10};
        std::function<void(std::span<std::byte>)> random;
    };

    class Listener {
    public:
        virtual ~Listener() = default;

        virtual void sendPacket(std::span<const std::byte> packet) = 0;
        virtual void onEstablished() = 0;
        virtual void onRestarted() = 0;
        virtual void onClosed(CloseReason reason) = 0;
        virtual void onMessage(Message message) = 0;
        virtual void onIncomingStreamsReset(std::span<const std::uint16_t> streams) = 0;
        virtual void onOutgoingStreamsReset(std::span<const std::uint16_t> streams, bool performed) = 0;
    };

    Association(Listener &listener, Config config);

    /**
     * @see RFC 9260 section 5.1
     */
    void connect(Clock::time_point now);
    void handlePacket(std::span<const std::byte> data, Clock::time_point now);
    void handleTimeout(Clock::time_point now);
    [[nodiscard]] std::optional<Clock::time_point> nextTimeout() const;

    bool send(std::uint16_t stream, std::uint32_t ppid, ByteBuf payload, const Sender::SendOptions &options,
              Clock::time_point now);

    /**
     * @see RFC 6525 section 5.1.2
     */
    bool resetStreams(std::span<const std::uint16_t> streams, Clock::time_point now);

    /**
     * @see RFC 9260 section 9.2
     */
    void shutdown(Clock::time_point now);

    /**
     * @see RFC 9260 section 9.1
     */
    void abort(Clock::time_point now);

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] std::uint16_t inboundStreams() const { return tcb_ ? tcb_->inbound_streams : 0; }
    [[nodiscard]] std::uint16_t outboundStreams() const { return tcb_ ? tcb_->outbound_streams : 0; }
    [[nodiscard]] bool supportsPartialReliability() const { return tcb_ && tcb_->partial_reliability; }
    [[nodiscard]] std::size_t bufferedAmount() const { return tcb_ ? tcb_->sender.bufferedAmount() : 0; }
    [[nodiscard]] std::size_t bufferedAmount(std::uint16_t stream) const
    {
        return tcb_ ? tcb_->sender.bufferedAmount(stream) : 0;
    }
    [[nodiscard]] std::size_t droppedPackets() const { return dropped_packets_; }
    [[nodiscard]] const Sender *sender() const { return tcb_ ? &tcb_->sender : nullptr; }

private:
    struct Capabilities {
        bool partial_reliability;
        bool reconfig;
    };

    struct ResetRequest {
        std::uint32_t sequence;
        std::uint32_t last_assigned_tsn;
        std::vector<std::uint16_t> streams;
        unsigned in_progress_answers{0};
        bool in_progress{false};
    };

    struct Tcb {
        std::uint32_t local_tag;
        std::uint32_t peer_tag;
        std::uint32_t local_initial_tsn;
        std::uint64_t tie_tag;
        std::uint16_t inbound_streams;
        std::uint16_t outbound_streams;
        bool partial_reliability;
        bool reconfig;
        Sender sender;
        Receiver receiver;
        std::uint32_t next_request_sequence;
        std::uint32_t last_peer_request;
        ReconfigResult last_peer_result{ReconfigResult::SuccessNothingToDo};
        std::optional<std::uint32_t> deferred_request;
        std::optional<ResetRequest> request;
        std::optional<Clock::time_point> reconfig_at;
        Clock::duration reconfig_backoff{};
        std::optional<Clock::time_point> heartbeat_at;
        std::optional<Clock::time_point> heartbeat_timeout;
        std::uint64_t heartbeat_nonce{0};
        unsigned errors{0};
    };

    struct Control {
        ChunkType type;
        std::uint8_t flags;
        std::vector<std::byte> value;
    };

    struct Retry {
        Clock::time_point at;
        Clock::duration backoff;
        unsigned count;
    };

    struct Established {};
    struct Restarted {};
    struct Closed {
        CloseReason reason;
    };
    struct IncomingReset {
        std::vector<std::uint16_t> streams;
    };
    struct OutgoingReset {
        std::vector<std::uint16_t> streams;
        bool performed;
    };
    using Event = std::variant<Established, Restarted, Closed, Message, IncomingReset, OutgoingReset>;

    struct InitParameters {
        std::optional<std::span<const std::byte>> state_cookie;
        Capabilities capabilities;
        std::vector<std::span<const std::byte>> unrecognized;
    };

    static InitParameters readParameters(std::span<const Tlv> parameters);
    void writeCapabilities(ByteWriter &writer) const;

    void dispatch(const Packet &packet, Clock::time_point now);
    bool verify(const Packet &packet) const;
    void handleInit(const Chunk &chunk, Clock::time_point now);
    void handleInitAck(const Chunk &chunk, Clock::time_point now);
    bool handleCookieEcho(const Packet &packet, const Chunk &chunk, Clock::time_point now);
    void handleData(const Chunk &chunk, std::vector<Message> &delivered);
    void handleSack(const Chunk &chunk, Clock::time_point now);
    void handleHeartbeatAck(const Chunk &chunk, Clock::time_point now);
    void handleError(const Chunk &chunk);
    void handleShutdown(Clock::time_point now);
    void handleShutdownAck(const Packet &packet);
    void handleForwardTsn(const Chunk &chunk, std::vector<Message> &delivered);
    void handleReconfig(const Chunk &chunk, Clock::time_point now);
    bool handleUnknown(const Chunk &chunk, std::vector<std::byte> &unrecognized);

    void establish(const StateCookie &cookie, Clock::time_point now);
    void becomeEstablished(Clock::time_point now);
    Tcb makeTcb(std::uint32_t local_tag, std::uint32_t local_initial_tsn, std::uint32_t peer_tag,
                std::uint32_t peer_initial_tsn, std::uint32_t peer_window, std::uint16_t peer_outbound,
                std::uint16_t peer_inbound, Capabilities capabilities);
    void sendInit();
    void sendCookieEcho();
    void sendStandalone(std::uint32_t verification_tag, ChunkType type, std::uint8_t flags,
                        std::span<const std::byte> value);
    void queue(ChunkType type, std::uint8_t flags, std::vector<std::byte> value);
    void queueResetRequest();
    void transmit(Clock::time_point now);
    void maybeFinishShutdown(Clock::time_point now);
    void sendShutdown(Clock::time_point now);
    void sendShutdownAck(Clock::time_point now);
    void countError();
    void fail(CauseCode code, CloseReason reason, std::span<const std::byte> value = {});
    void close(CloseReason reason);
    void deliver();

    template <typename T>
    T random();

    Listener &listener_;
    Config config_;
    StateCookie::Key cookie_key_;
    State state_{State::Closed};
    bool terminated_{false};
    std::uint32_t init_tag_{0};
    std::uint32_t init_tsn_{0};
    std::optional<Retry> t1_;
    std::optional<Retry> t2_;
    std::vector<std::byte> cookie_echo_;
    std::optional<Tcb> tcb_;
    std::deque<Control> control_;
    std::vector<std::byte> buffer_;
    std::deque<Event> events_;
    bool delivering_{false};
    std::size_t dropped_packets_{0};
};

}  // namespace endgate::rtc::sctp
