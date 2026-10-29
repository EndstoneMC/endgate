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

namespace {

StunMessage::TransactionId newTransactionId()
{
    StunMessage::TransactionId transaction_id{};
    if (RAND_bytes(reinterpret_cast<unsigned char *>(transaction_id.data()), static_cast<int>(transaction_id.size())) !=
        1) {
        throw std::runtime_error("RAND_bytes failed to generate a STUN transaction id");
    }
    return transaction_id;
}

std::uint32_t hostPriority(std::size_t index)
{
    constexpr std::uint32_t host_type_preference = 126;
    return (host_type_preference << 24U) | ((65535U - static_cast<std::uint32_t>(index)) << 8U) | 255U;
}

}  // namespace

std::uint64_t IceController::generateTieBreaker()
{
    std::uint64_t tie_breaker = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char *>(&tie_breaker), sizeof(tie_breaker)) != 1) {
        throw std::runtime_error("RAND_bytes failed to generate an ICE tie-breaker");
    }
    return tie_breaker;
}

IceController::IceController(Listener &listener, IceCredentials local, std::string remote_ufrag, std::string remote_pwd,
                             std::span<const asio::ip::udp::endpoint> remote_candidates, std::uint64_t tie_breaker,
                             Config config, std::chrono::steady_clock::time_point now)
    : listener_(listener), local_(std::move(local)), remote_ufrag_(std::move(remote_ufrag)),
      remote_pwd_(std::move(remote_pwd)), tie_breaker_(tie_breaker), config_(config),
      negotiation_deadline_(now + config.negotiation_timeout)
{
    std::size_t index = 0;
    for (const auto &remote : remote_candidates) {
        if (pairs_.size() >= config_.max_candidate_pairs) {
            break;
        }
        if (findByRemote(remote) == nullptr) {
            pairs_.push_back({.remote = remote,
                              .priority = hostPriority(index),
                              .next_transmit = now + config_.check_pacing * index,
                              .last_response = now});
            ++index;
        }
    }
}

void IceController::addRemoteCandidate(std::chrono::steady_clock::time_point now, const asio::ip::udp::endpoint &remote)
{
    if (state_ == State::Failed || pairs_.size() >= config_.max_candidate_pairs || findByRemote(remote) != nullptr) {
        return;
    }
    pairs_.push_back(
        {.remote = remote, .priority = hostPriority(pairs_.size()), .next_transmit = now, .last_response = now});
}

IceController::CandidatePair *IceController::findByRemote(const asio::ip::udp::endpoint &remote)
{
    const auto pair = std::ranges::find(pairs_, remote, &CandidatePair::remote);
    return pair == pairs_.end() ? nullptr : &*pair;
}

IceController::CandidatePair *IceController::findByTransaction(const StunMessage::TransactionId &transaction_id)
{
    const auto pair = std::ranges::find_if(pairs_, [&](const CandidatePair &candidate) {
        return (candidate.state == CheckState::InProgress || candidate.state == CheckState::Succeeded) &&
               candidate.transaction_id == transaction_id;
    });
    return pair == pairs_.end() ? nullptr : &*pair;
}

void IceController::handleDatagram(std::chrono::steady_clock::time_point now, const asio::ip::udp::endpoint &remote,
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

    if (message->message_class == StunClass::Request) {
        const auto expected_username = local_.ufrag + ":" + remote_ufrag_;
        if (!message->fingerprint || message->username != expected_username ||
            !message->verifyIntegrity(datagram, local_.pwd)) {
            ++statistics_.datagrams_dropped;
            return;
        }
        if (!message->unknown_comprehension_required.empty()) {
            respondWithError(remote, *message, {.code = 420, .reason = "Unknown Attribute"},
                             message->unknown_comprehension_required);
            return;
        }
        if (role_ == Role::Controlling && message->ice_controlling) {
            ++statistics_.role_conflicts;
            if (tie_breaker_ >= *message->ice_controlling) {
                respondWithError(remote, *message, {.code = 487, .reason = "Role Conflict"}, {});
                return;
            }
            role_ = Role::Controlled;
        }
        respond(remote, *message);
        ++statistics_.checks_answered;
        if (auto *const pair = findByRemote(remote); pair != nullptr && message->use_candidate) {
            select(remote);
        }
        return;
    }

    auto *const pair = findByTransaction(message->transaction_id);
    if (pair == nullptr || !message->fingerprint || !message->verifyIntegrity(datagram, remote_pwd_)) {
        ++statistics_.datagrams_dropped;
        return;
    }
    if (message->message_class == StunClass::ErrorResponse) {
        if (message->error_code && message->error_code->code == 487) {
            ++statistics_.role_conflicts;
            role_ = role_ == Role::Controlling ? Role::Controlled : Role::Controlling;
            pair->state = CheckState::Waiting;
            pair->nominating = false;
            pair->transmits = 0;
            pair->next_transmit = now;
            return;
        }
        pair->state = CheckState::Failed;
        return;
    }
    onResponse(now, *pair);
}

void IceController::onResponse(std::chrono::steady_clock::time_point now, CandidatePair &pair)
{
    ++statistics_.responses_accepted;
    pair.last_response = now;
    const auto was_use_candidate = pair.use_candidate;
    pair.state = CheckState::Succeeded;
    pair.transmits = 0;
    if (was_use_candidate) {
        select(pair.remote);
    }
    else if (!nominated_ && role_ == Role::Controlling) {
        nominate(now, pair);
    }
}

void IceController::nominate(std::chrono::steady_clock::time_point now, CandidatePair &pair)
{
    nominated_ = true;
    pair.nominating = true;
    pair.transmits = 0;
    sendCheck(now, pair, true);
}

void IceController::select(const asio::ip::udp::endpoint &remote)
{
    if (selected_ != remote) {
        selected_ = remote;
        listener_.onSelectedPairChanged(selected_);
    }
    setState(State::Connected);
}

void IceController::sendCheck(std::chrono::steady_clock::time_point now, CandidatePair &pair, bool use_candidate)
{
    StunMessage request;
    request.message_class = StunClass::Request;
    request.method = StunMethod::Binding;
    request.transaction_id = newTransactionId();
    request.username = remote_ufrag_ + ":" + local_.ufrag;
    request.priority = pair.priority;
    if (role_ == Role::Controlling) {
        request.ice_controlling = tie_breaker_;
    }
    else {
        request.ice_controlled = tie_breaker_;
    }
    request.use_candidate = use_candidate;

    pair.transaction_id = request.transaction_id;
    pair.use_candidate = use_candidate;
    pair.state = CheckState::InProgress;
    const auto backoff = std::min(config_.initial_rto * (1U << std::min(pair.transmits, 10U)), config_.max_rto);
    pair.next_transmit = now + backoff;
    ++pair.transmits;
    ++statistics_.checks_sent;
    listener_.sendIceDatagram(pair.remote, request.encode(remote_pwd_));
}

void IceController::respond(const asio::ip::udp::endpoint &remote, const StunMessage &request)
{
    StunMessage response;
    response.message_class = StunClass::SuccessResponse;
    response.transaction_id = request.transaction_id;
    response.xor_mapped_address = remote;
    if (remote.address().is_v6() && remote.address().to_v6().is_v4_mapped()) {
        response.xor_mapped_address = asio::ip::udp::endpoint(
            asio::ip::make_address_v4(asio::ip::v4_mapped, remote.address().to_v6()), remote.port());
    }
    listener_.sendIceDatagram(remote, response.encode(local_.pwd));
}

void IceController::respondWithError(const asio::ip::udp::endpoint &remote, const StunMessage &request,
                                     StunErrorCode error, std::vector<std::uint16_t> unknown_attributes)
{
    StunMessage response;
    response.message_class = StunClass::ErrorResponse;
    response.transaction_id = request.transaction_id;
    response.error_code = std::move(error);
    response.unknown_attributes = std::move(unknown_attributes);
    listener_.sendIceDatagram(remote, response.encode(local_.pwd));
}

bool IceController::send(std::span<const std::byte> datagram)
{
    if (!selected_ || state_ != State::Connected) {
        return false;
    }
    listener_.sendIceDatagram(*selected_, datagram);
    return true;
}

std::optional<std::chrono::steady_clock::time_point> IceController::nextTimeout() const
{
    if (state_ == State::Failed) {
        return std::nullopt;
    }
    std::optional<std::chrono::steady_clock::time_point> next;
    const auto earlier = [&next](std::chrono::steady_clock::time_point candidate) {
        next = next ? std::min(*next, candidate) : candidate;
    };
    if (state_ == State::Checking) {
        earlier(negotiation_deadline_);
    }
    if (selected_) {
        earlier(pairs_.empty() ? negotiation_deadline_ : pairs_.front().last_response + config_.consent_timeout);
    }
    for (const auto &pair : pairs_) {
        if (pair.state == CheckState::Waiting || pair.state == CheckState::InProgress) {
            earlier(pair.next_transmit);
        }
        if (selected_ && pair.remote == *selected_) {
            earlier(pair.last_response + config_.consent_interval);
            earlier(pair.last_response + config_.consent_timeout);
        }
    }
    return next;
}

void IceController::handleTimeout(std::chrono::steady_clock::time_point now)
{
    if (state_ == State::Failed) {
        return;
    }

    for (auto &pair : pairs_) {
        if ((pair.state != CheckState::Waiting && pair.state != CheckState::InProgress) || now < pair.next_transmit) {
            continue;
        }
        if (pair.transmits >= config_.max_check_transmits) {
            pair.state = CheckState::Failed;
            pair.nominating = false;
            continue;
        }
        sendCheck(now, pair, pair.use_candidate && pair.nominating);
    }

    if (selected_) {
        if (auto *const pair = findByRemote(*selected_); pair != nullptr) {
            if (now - pair->last_response >= config_.consent_timeout) {
                setState(State::Failed);
                return;
            }
            if (pair->state == CheckState::Succeeded && now - pair->last_response >= config_.consent_interval) {
                pair->transmits = 0;
                sendCheck(now, *pair, false);
            }
        }
    }

    if (state_ == State::Checking) {
        const auto pending = std::ranges::any_of(pairs_, [](const CandidatePair &pair) {
            return pair.state == CheckState::Waiting || pair.state == CheckState::InProgress;
        });
        if (now >= negotiation_deadline_ || !pending) {
            setState(State::Failed);
        }
    }
}

void IceController::setState(State state)
{
    if (state_ != state) {
        state_ = state;
        listener_.onIceStateChanged(state);
    }
}

}  // namespace endgate::rtc
