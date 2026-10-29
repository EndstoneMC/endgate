#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/rtc/certificate.h"

namespace endgate::rtc {

/**
 * DTLS 1.2 for one PeerConnection, carrying its SCTP packets. Every record OpenSSL writes leaves as
 * its own datagram, and every datagram that arrives is handed to OpenSSL on its own.
 *
 * @see rtc::impl::DtlsTransport
 * @see RFC 8261
 */
class DtlsTransport {
public:
    enum class Role {
        Client,
        Server,
    };

    enum class State {
        New,
        Connecting,
        Connected,
        Closed,
        Failed,
    };

    struct Config {
        unsigned int mtu{1200};
        std::chrono::milliseconds initial_retransmission_timeout{std::chrono::seconds{1}};
        std::chrono::milliseconds handshake_timeout{std::chrono::seconds{30}};
    };

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void sendDtlsDatagram(ByteBufView datagram) = 0;
        virtual void onDtlsData(ByteBufView data) = 0;
        virtual void onDtlsStateChanged(State state) = 0;
    };

    DtlsTransport(Listener &listener, std::shared_ptr<const Certificate> certificate, Role role,
                  std::vector<CertificateFingerprint> remote_fingerprints, Config config);

    DtlsTransport(const DtlsTransport &) = delete;
    DtlsTransport &operator=(const DtlsTransport &) = delete;
    DtlsTransport(DtlsTransport &&) = delete;
    DtlsTransport &operator=(DtlsTransport &&) = delete;
    ~DtlsTransport() = default;

    [[nodiscard]] Role getRole() const { return role_; }
    [[nodiscard]] State getState() const { return state_; }

    void start(std::chrono::steady_clock::time_point now);
    void handleDatagram(std::chrono::steady_clock::time_point now, ByteBufView datagram);
    bool send(ByteBufView data);

    /**
     * @see RFC 5246 section 7.2.1
     */
    void close();

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> nextTimeout() const;
    void handleTimeout(std::chrono::steady_clock::time_point now);

private:
    void advance(int result);
    void readApplicationData();
    void flush();
    void rearm(std::chrono::steady_clock::time_point now);
    void setState(State state);

    Listener &listener_;
    std::shared_ptr<const Certificate> certificate_;
    Role role_;
    std::vector<CertificateFingerprint> remote_fingerprints_;
    Config config_;
    std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> datagram_method_;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context_;
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl_;
    BIO *incoming_{nullptr};
    std::vector<ByteBuf> outgoing_;
    ByteBuf read_buffer_;
    std::optional<std::chrono::steady_clock::time_point> handshake_deadline_;
    std::optional<std::chrono::steady_clock::time_point> retransmission_deadline_;
    State state_{State::New};
};

}  // namespace endgate::rtc
