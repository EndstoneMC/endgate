#include "endgate/network/rtc/ice.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <openssl/rand.h>

namespace endgate::rtc {

IceCredentials IceCredentials::generate()
{
    constexpr std::string_view ice_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    constexpr std::size_t ufrag_length = 8;
    constexpr std::size_t pwd_length = 24;
    std::array<unsigned char, ufrag_length + pwd_length> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed to generate ICE credentials");
    }
    IceCredentials credentials;
    for (std::size_t i = 0; i < random.size(); ++i) {
        (i < ufrag_length ? credentials.ufrag : credentials.pwd).push_back(ice_chars[random[i] & 0x3FU]);
    }
    return credentials;
}

IceTransport::IceTransport(Listener &listener, IceCredentials local, std::string remote_ufrag, Config config,
                           std::chrono::steady_clock::time_point now)
    : listener_(listener), local_(std::move(local)), remote_ufrag_(std::move(remote_ufrag)), config_(config),
      negotiation_deadline_(now + config.negotiation_timeout)
{
}

void IceTransport::handleDatagram(std::chrono::steady_clock::time_point now, const asio::ip::udp::endpoint &remote,
                                  std::span<const std::byte> datagram)
{
    const auto message = StunMessage::decode(datagram);
    if (state_ == State::Failed || !message || message->method != StunMethod::Binding) {
        ++statistics_.datagrams_dropped;
        return;
    }
    if (message->message_class == StunClass::Indication) {
        return;
    }

    const auto expected_username = local_.ufrag + ":" + remote_ufrag_;
    if (message->message_class != StunClass::Request || !message->fingerprint ||
        message->username != expected_username || !message->verifyIntegrity(datagram, local_.pwd)) {
        ++statistics_.datagrams_dropped;
        return;
    }

    if (!message->unknown_comprehension_required.empty()) {
        respondWithError(remote, *message, {.code = 420, .reason = "Unknown Attribute"},
                         message->unknown_comprehension_required);
        return;
    }
    if (message->ice_controlled && message->ice_controlling) {
        respondWithError(remote, *message, {.code = 400, .reason = "Bad Request"}, {});
        return;
    }
    if (message->ice_controlled) {
        respondWithError(remote, *message, {.code = 487, .reason = "Role Conflict"}, {});
        return;
    }

    StunMessage response;
    response.message_class = StunClass::SuccessResponse;
    response.transaction_id = message->transaction_id;
    response.xor_mapped_address = remote;
    if (remote.address().is_v6() && remote.address().to_v6().is_v4_mapped()) {
        response.xor_mapped_address = asio::ip::udp::endpoint(
            asio::ip::make_address_v4(asio::ip::v4_mapped, remote.address().to_v6()), remote.port());
    }
    listener_.sendIceDatagram(remote, response.encode(local_.pwd));
    ++statistics_.checks_answered;

    auto pair = std::ranges::find(pairs_, remote, &CandidatePair::remote);
    if (pair == pairs_.end()) {
        if (pairs_.size() >= config_.max_candidate_pairs) {
            const auto evicted = std::ranges::min_element(pairs_, [this](const auto &lhs, const auto &rhs) {
                return std::pair{lhs.remote == selected_, lhs.last_check} <
                       std::pair{rhs.remote == selected_, rhs.last_check};
            });
            if (evicted == pairs_.end() || evicted->remote == selected_) {
                return;
            }
            const auto removed = evicted->remote;
            pairs_.erase(evicted);
            listener_.onCandidatePairRemoved(removed);
        }
        pairs_.push_back({.remote = remote, .last_check = now, .nominated_at = std::nullopt, .priority = 0});
        pair = std::prev(pairs_.end());
        listener_.onCandidatePairAdded(remote);
    }
    pair->last_check = now;
    pair->priority = message->priority.value_or(pair->priority);

    if (message->use_candidate) {
        pair->nominated_at = now;
        if (selected_ != remote) {
            selected_ = remote;
            listener_.onSelectedPairChanged(selected_);
        }
        setState(State::Connected);
    }
}

bool IceTransport::send(std::span<const std::byte> datagram)
{
    if (!selected_ || state_ != State::Connected) {
        return false;
    }
    listener_.sendIceDatagram(*selected_, datagram);
    return true;
}

std::optional<std::chrono::steady_clock::time_point> IceTransport::nextTimeout() const
{
    if (state_ == State::Failed) {
        return std::nullopt;
    }
    std::optional<std::chrono::steady_clock::time_point> next;
    if (state_ == State::Checking) {
        next = negotiation_deadline_;
    }
    for (const auto &pair : pairs_) {
        const auto expiry = pair.last_check + config_.consent_timeout;
        next = next ? std::min(*next, expiry) : expiry;
    }
    return next;
}

void IceTransport::handleTimeout(std::chrono::steady_clock::time_point now)
{
    if (state_ == State::Failed) {
        return;
    }
    const auto expired = std::ranges::stable_partition(
        pairs_, [&](const CandidatePair &pair) { return now < pair.last_check + config_.consent_timeout; });
    std::vector<asio::ip::udp::endpoint> removed;
    for (const auto &pair : expired) {
        removed.push_back(pair.remote);
    }
    pairs_.erase(expired.begin(), expired.end());
    for (const auto &remote : removed) {
        listener_.onCandidatePairRemoved(remote);
    }
    if (selected_ && std::ranges::contains(removed, *selected_)) {
        reselect();
    }

    if (state_ == State::Checking) {
        if (now >= negotiation_deadline_) {
            setState(State::Failed);
        }
    }
    else if (pairs_.empty()) {
        setState(State::Failed);
    }
    else {
        setState(selected_ ? State::Connected : State::Disconnected);
    }
}

void IceTransport::respondWithError(const asio::ip::udp::endpoint &remote, const StunMessage &request,
                                    StunErrorCode error, std::vector<std::uint16_t> unknown_attributes)
{
    StunMessage response;
    response.message_class = StunClass::ErrorResponse;
    response.transaction_id = request.transaction_id;
    response.error_code = std::move(error);
    response.unknown_attributes = std::move(unknown_attributes);
    listener_.sendIceDatagram(remote, response.encode(local_.pwd));
}

void IceTransport::setState(State state)
{
    if (state_ != state) {
        state_ = state;
        listener_.onIceStateChanged(state);
    }
}

void IceTransport::reselect()
{
    const auto latest = std::ranges::max_element(pairs_, {}, &CandidatePair::nominated_at);
    selected_ = latest != pairs_.end() && latest->nominated_at ? std::optional{latest->remote} : std::nullopt;
    listener_.onSelectedPairChanged(selected_);
}

}  // namespace endgate::rtc
