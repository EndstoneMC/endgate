#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>

#include "endgate/network/buffer.h"
#include "endgate/network/rtc/sctp/association.h"
#include "endgate/network/rtc/sctp/dcep.h"

namespace endgate::rtc {

/**
 * @see rtc::impl::DataChannel
 */
struct DataChannel {
    std::uint16_t stream;
    std::string label;
    std::string protocol;
    Reliability reliability;
    std::uint16_t priority;
};

/**
 * WebRTC data channels over one SCTP association, which DTLS carries. Channels are opened in band with DCEP and closed
 * by resetting their streams; each delivers whole messages, never fragments.
 *
 * Channels the peer opens are reported through onChannelOpen once they are acknowledged. A channel opened here is
 * usable as soon as createDataChannel returns.
 *
 * @see RFC 8831
 * @see RFC 8832
 * @see rtc::impl::SctpTransport
 */
class SctpTransport : sctp::Association::Listener {
public:
    using Clock = std::chrono::steady_clock;

    enum class State {
        Connecting,
        Connected,
        Disconnected,
        Failed,
    };

    struct Config {
        bool dtls_client;
        std::size_t remote_max_message_size{65536};
        std::size_t max_channels{16};
        std::size_t max_label_size{1024};
        sctp::Association::Config association{};
    };

    class Listener {
    public:
        virtual ~Listener() = default;

        virtual void sendPacket(std::span<const std::byte> packet) = 0;
        virtual void onStateChange(State state) = 0;
        virtual void onChannelOpen(const DataChannel &channel) = 0;
        virtual void onChannelMessage(const DataChannel &channel, ByteBuf message, bool binary) = 0;
        virtual void onChannelClosed(const DataChannel &channel) = 0;
    };

    SctpTransport(Listener &listener, Config config);

    void start(Clock::time_point now);
    void handlePacket(std::span<const std::byte> packet, Clock::time_point now);
    void handleTimeout(Clock::time_point now);
    [[nodiscard]] std::optional<Clock::time_point> nextTimeout() const { return association_.nextTimeout(); }

    std::optional<std::uint16_t> createDataChannel(std::string label, std::string protocol,
                                                   const Reliability &reliability, std::uint16_t priority,
                                                   Clock::time_point now);
    bool send(std::uint16_t stream, ByteBuf message, bool binary, Clock::time_point now);

    /**
     * @see RFC 8831 section 6.7
     */
    void closeDataChannel(std::uint16_t stream, Clock::time_point now);
    void close(Clock::time_point now);
    void abort(Clock::time_point now);

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] std::size_t maxMessageSize() const { return config_.remote_max_message_size; }
    [[nodiscard]] std::size_t bufferedAmount(std::uint16_t stream) const { return association_.bufferedAmount(stream); }
    [[nodiscard]] const DataChannel *channel(std::uint16_t stream) const;
    [[nodiscard]] const sctp::Association &association() const { return association_; }

private:
    struct Channel {
        DataChannel info;
        bool acknowledged;
        bool closing{false};
        bool outgoing_reset{false};
        bool incoming_reset{false};
    };

    void sendPacket(std::span<const std::byte> packet) override { listener_.sendPacket(packet); }
    void onEstablished() override;
    void onRestarted() override;
    void onClosed(sctp::Association::CloseReason reason) override;
    void onMessage(sctp::Message message) override;
    void onIncomingStreamsReset(std::span<const std::uint16_t> streams) override;
    void onOutgoingStreamsReset(std::span<const std::uint16_t> streams, bool performed) override;

    void handleOpen(std::uint16_t stream, std::span<const std::byte> message);
    void refuse(std::uint16_t stream);
    void finish(std::uint16_t stream);
    void closeAll();
    void changeState(State state);

    Listener &listener_;
    Config config_;
    State state_{State::Connecting};
    std::map<std::uint16_t, Channel> channels_;
    std::optional<std::uint16_t> notifying_;
    bool finish_after_notify_{false};
    Clock::time_point now_{};
    sctp::Association association_;
};

}  // namespace endgate::rtc
