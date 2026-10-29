#include "endgate/network/rtc/dtls.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <openssl/err.h>
#include <openssl/x509_vfy.h>

namespace endgate::rtc {

DtlsTransport::DtlsTransport(Listener &listener, std::shared_ptr<const Certificate> certificate, Role role,
                             std::vector<CertificateFingerprint> remote_fingerprints, Config config)
    : listener_(listener), certificate_(std::move(certificate)), role_(role),
      remote_fingerprints_(std::move(remote_fingerprints)), config_(config),
      datagram_method_(BIO_meth_new(BIO_TYPE_BIO, "endgate datagram"), BIO_meth_free),
      context_(SSL_CTX_new(DTLS_method()), SSL_CTX_free), ssl_(nullptr, SSL_free),
      read_buffer_(Unpooled::buffer(SSL3_RT_MAX_PLAIN_LENGTH))
{
    if (!datagram_method_ || !context_ || !certificate_) {
        throw std::runtime_error("failed to create the DTLS context");
    }
    BIO_meth_set_create(datagram_method_.get(), [](BIO *bio) {
        BIO_set_init(bio, 1);
        return 1;
    });
    BIO_meth_set_write_ex(datagram_method_.get(),
                          [](BIO *bio, const char *data, std::size_t length, std::size_t *written) {
                              auto *self = static_cast<DtlsTransport *>(BIO_get_data(bio));
                              const auto *bytes = reinterpret_cast<const std::byte *>(data);
                              self->outgoing_.push_back(Unpooled::copiedBuffer({bytes, length}));
                              *written = length;
                              return 1;
                          });
    BIO_meth_set_ctrl(datagram_method_.get(), [](BIO *, int command, auto argument, void *) {
        return static_cast<decltype(argument)>(command == BIO_CTRL_FLUSH);
    });

    auto *context = context_.get();
    SSL_CTX_set_min_proto_version(context, DTLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(context, DTLS1_2_VERSION);
    SSL_CTX_set_options(context,
                        SSL_OP_NO_QUERY_MTU | SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION | SSL_OP_NO_TICKET);
    SSL_CTX_set_read_ahead(context, 1);
    SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, [](int, X509_STORE_CTX *store) {
        if (X509_STORE_CTX_get_error_depth(store) != 0) {
            return 1;
        }
        auto *ssl = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
        const auto *self = static_cast<const DtlsTransport *>(SSL_get_app_data(ssl));
        return CertificateFingerprint::matches(X509_STORE_CTX_get_current_cert(store), self->remote_fingerprints_) ? 1
                                                                                                                   : 0;
    });
    if (SSL_CTX_set_cipher_list(context, "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:"
                                         "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-AES128-GCM-SHA256:"
                                         "ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-CHACHA20-POLY1305:"
                                         "ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES128-SHA") != 1 ||
        SSL_CTX_set1_groups_list(context, "X25519:P-256:P-384") != 1 ||
        SSL_CTX_set_tlsext_use_srtp(context, "SRTP_AEAD_AES_256_GCM:SRTP_AEAD_AES_128_GCM:SRTP_AES128_CM_SHA1_80") !=
            0 ||
        SSL_CTX_use_certificate(context, certificate_->getX509()) != 1 ||
        SSL_CTX_use_PrivateKey(context, certificate_->getPrivateKey()) != 1 ||
        SSL_CTX_check_private_key(context) != 1) {
        throw std::runtime_error("failed to configure the DTLS context");
    }

    ssl_.reset(SSL_new(context));
    incoming_ = BIO_new(BIO_s_mem());
    BIO *outgoing = BIO_new(datagram_method_.get());
    if (!ssl_ || incoming_ == nullptr || outgoing == nullptr) {
        BIO_free(incoming_);
        BIO_free(outgoing);
        throw std::runtime_error("failed to create the DTLS session");
    }
    BIO_set_mem_eof_return(incoming_, -1);
    BIO_set_data(outgoing, this);
    SSL_set_bio(ssl_.get(), incoming_, outgoing);
    SSL_set_app_data(ssl_.get(), this);
    SSL_set_mtu(ssl_.get(), config_.mtu);
    DTLS_set_timer_cb(ssl_.get(), [](SSL *ssl, unsigned int previous_us) -> unsigned int {
        const auto *self = static_cast<const DtlsTransport *>(SSL_get_app_data(ssl));
        constexpr unsigned int maximum_us = 60'000'000;
        const auto initial_us = static_cast<unsigned int>(
            std::chrono::duration_cast<std::chrono::microseconds>(self->config_.initial_retransmission_timeout)
                .count());
        return previous_us == 0 ? initial_us : std::min(previous_us * 2, maximum_us);
    });
    if (role_ == Role::Client) {
        SSL_set_connect_state(ssl_.get());
    }
    else {
        SSL_set_accept_state(ssl_.get());
    }
}

void DtlsTransport::start(std::chrono::steady_clock::time_point now)
{
    if (state_ != State::New) {
        return;
    }
    handshake_deadline_ = now + config_.handshake_timeout;
    setState(State::Connecting);
    ERR_clear_error();
    advance(SSL_do_handshake(ssl_.get()));
    flush();
    rearm(now);
}

void DtlsTransport::handleDatagram(std::chrono::steady_clock::time_point now, ByteBufView datagram)
{
    start(now);
    if ((state_ != State::Connecting && state_ != State::Connected) || !datagram.isReadable()) {
        return;
    }
    ERR_clear_error();
    BIO_write(incoming_, datagram.nioBuffer().data(), static_cast<int>(datagram.readableBytes()));
    if (state_ == State::Connecting) {
        advance(SSL_do_handshake(ssl_.get()));
    }
    else {
        readApplicationData();
    }
    BIO_reset(incoming_);
    flush();
    rearm(now);
}

bool DtlsTransport::send(ByteBufView data)
{
    if (state_ != State::Connected) {
        return false;
    }
    ERR_clear_error();
    std::size_t written = 0;
    const auto result = SSL_write_ex(ssl_.get(), data.nioBuffer().data(), data.readableBytes(), &written);
    if (result != 1) {
        const auto error = SSL_get_error(ssl_.get(), result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            setState(State::Failed);
        }
    }
    flush();
    return result == 1;
}

void DtlsTransport::close()
{
    if (state_ != State::Connecting && state_ != State::Connected) {
        return;
    }
    ERR_clear_error();
    SSL_shutdown(ssl_.get());
    flush();
    setState(State::Closed);
}

std::optional<std::chrono::steady_clock::time_point> DtlsTransport::nextTimeout() const
{
    if (state_ != State::Connecting && state_ != State::Connected) {
        return std::nullopt;
    }
    if (state_ == State::Connecting && handshake_deadline_ &&
        (!retransmission_deadline_ || *handshake_deadline_ < *retransmission_deadline_)) {
        return handshake_deadline_;
    }
    return retransmission_deadline_;
}

void DtlsTransport::handleTimeout(std::chrono::steady_clock::time_point now)
{
    if (state_ != State::Connecting && state_ != State::Connected) {
        return;
    }
    if (state_ == State::Connecting && handshake_deadline_ && now >= *handshake_deadline_) {
        setState(State::Failed);
        return;
    }
    if (retransmission_deadline_ && now >= *retransmission_deadline_) {
        ERR_clear_error();
        if (DTLSv1_handle_timeout(ssl_.get()) < 0) {
            setState(State::Failed);
        }
    }
    flush();
    rearm(now);
}

void DtlsTransport::advance(int result)
{
    if (result == 1) {
        setState(State::Connected);
        readApplicationData();
        return;
    }
    const auto error = SSL_get_error(ssl_.get(), result);
    if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
        setState(State::Failed);
    }
}

void DtlsTransport::readApplicationData()
{
    while (state_ == State::Connected) {
        std::size_t read = 0;
        read_buffer_.clear();
        const auto result = SSL_read_ex(ssl_.get(), read_buffer_.nioBuffer(0, read_buffer_.capacity()).data(),
                                        read_buffer_.capacity(), &read);
        if (result == 1) {
            read_buffer_.writerIndex(read);
            listener_.onDtlsData(read_buffer_.slice());
            continue;
        }
        const auto error = SSL_get_error(ssl_.get(), result);
        if (error == SSL_ERROR_ZERO_RETURN) {
            SSL_shutdown(ssl_.get());
            setState(State::Closed);
        }
        else if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            setState(State::Failed);
        }
        return;
    }
}

void DtlsTransport::flush()
{
    auto datagrams = std::exchange(outgoing_, {});
    for (const auto &datagram : datagrams) {
        listener_.sendDtlsDatagram(datagram.slice());
    }
}

void DtlsTransport::rearm(std::chrono::steady_clock::time_point now)
{
    timeval remaining{};
    retransmission_deadline_.reset();
    if ((state_ == State::Connecting || state_ == State::Connected) &&
        DTLSv1_get_timeout(ssl_.get(), &remaining) == 1) {
        retransmission_deadline_ =
            now + std::chrono::seconds{remaining.tv_sec} + std::chrono::microseconds{remaining.tv_usec};
    }
}

void DtlsTransport::setState(State state)
{
    if (state_ != state) {
        state_ = state;
        listener_.onDtlsStateChanged(state);
    }
}

}  // namespace endgate::rtc
