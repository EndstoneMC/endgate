#include "endgate/network/nethernet/manager.h"

#include <algorithm>
#include <utility>

#include "endgate/network/rtc/stun.h"

namespace endgate::nethernet {

NetworkSessionManager::Entry::Entry(NetworkSessionManager &manager, SessionId id, std::string network_id,
                                    std::string ufrag, const rtc::Description &offer, rtc::IceCredentials local,
                                    Clock::time_point now)
    : manager(manager), id(id), network_id(std::move(network_id)), ufrag(std::move(ufrag)),
      session(*this, offer, std::move(local), manager.config_.certificate, manager.config_.session, now)
{
}

void NetworkSessionManager::Entry::sendDatagram(const asio::ip::udp::endpoint &remote,
                                                std::span<const std::byte> datagram)
{
    manager.listener_.sendDatagram(remote, datagram);
}

void NetworkSessionManager::Entry::onRouteAdded(const asio::ip::udp::endpoint &remote)
{
    manager.routes_[remote] = id;
}

void NetworkSessionManager::Entry::onRouteRemoved(const asio::ip::udp::endpoint &remote)
{
    if (const auto route = manager.routes_.find(remote); route != manager.routes_.end() && route->second == id) {
        manager.routes_.erase(route);
    }
}

void NetworkSessionManager::Entry::onOpen()
{
    manager.listener_.onSessionOpen(id, network_id);
}

void NetworkSessionManager::Entry::onPacket(ByteBuf packet)
{
    manager.listener_.onPacket(id, std::move(packet));
}

void NetworkSessionManager::Entry::onClosed(ESessionError reason)
{
    closed = reason;
}

NetworkSessionManager::NetworkSessionManager(Listener &listener, Config config)
    : listener_(listener), config_(std::move(config))
{
}

std::expected<std::string, ESessionError> NetworkSessionManager::join(const JoinRequest &request,
                                                                      std::span<const rtc::CandidateAddress> candidates,
                                                                      Clock::time_point now,
                                                                      std::chrono::system_clock::time_point wall_clock)
{
    if (sessions_.size() >= config_.max_sessions) {
        return std::unexpected(ESessionError::IncomingConnectionIgnored);
    }
    if (config_.require_identity && !IdentityAssertion::extractFromSdp(request.offer)) {
        return std::unexpected(ESessionError::IdentityNotAllowed);
    }
    const auto offer = rtc::Description::parse(IdentityAssertion::stripFromSdp(request.offer));
    if (!offer) {
        return std::unexpected(ESessionError::FailedToSetRemoteDescription);
    }

    auto local = rtc::IceCredentials::generate();
    while (ufrags_.contains(local.ufrag)) {
        local = rtc::IceCredentials::generate();
    }
    const auto id = next_id_++;
    auto sdp = rtc::Description::answer(*offer, local, config_.certificate->getFingerprint(), candidates).generate(id);
    if (config_.identity) {
        const auto identity = IdentityAssertion::create(*config_.identity, sdp, wall_clock);
        if (!identity) {
            return std::unexpected(ESessionError::FailedToCreateIdentityAssertion);
        }
        sdp = identity->injectIntoSdp(sdp);
    }

    const auto ufrag = local.ufrag;
    ++depth_;
    sessions_.emplace(id, std::make_unique<Entry>(*this, id, request.network_id, ufrag, *offer, std::move(local), now));
    ufrags_.emplace(ufrag, id);
    --depth_;
    reap();
    return sdp;
}

bool NetworkSessionManager::handleDatagram(Clock::time_point now, const asio::ip::udp::endpoint &remote,
                                           std::span<const std::byte> datagram)
{
    std::optional<SessionId> owner;
    if (const auto route = routes_.find(remote); route != routes_.end()) {
        owner = route->second;
    }
    else if (const auto ufrag = rtc::StunMessage::peekLocalUfrag(datagram)) {
        if (const auto known = ufrags_.find(std::string(*ufrag)); known != ufrags_.end()) {
            owner = known->second;
        }
    }
    const auto entry = owner ? sessions_.find(*owner) : sessions_.end();
    if (entry == sessions_.end()) {
        return false;
    }
    ++depth_;
    entry->second->session.handleDatagram(now, remote, datagram);
    --depth_;
    reap();
    return true;
}

bool NetworkSessionManager::send(SessionId session, const ByteBuf &packet, ESendType type, Clock::time_point now)
{
    const auto entry = sessions_.find(session);
    if (entry == sessions_.end()) {
        return false;
    }
    ++depth_;
    const auto sent = entry->second->session.send(packet, type, now);
    --depth_;
    reap();
    return sent;
}

void NetworkSessionManager::close(SessionId session, Clock::time_point now)
{
    const auto entry = sessions_.find(session);
    if (entry == sessions_.end()) {
        return;
    }
    ++depth_;
    entry->second->session.close(now);
    --depth_;
    reap();
}

std::optional<NetworkSessionManager::Clock::time_point> NetworkSessionManager::nextTimeout() const
{
    std::optional<Clock::time_point> earliest;
    for (const auto &[id, entry] : sessions_) {
        if (const auto deadline = entry->session.nextTimeout(); deadline && (!earliest || *deadline < *earliest)) {
            earliest = deadline;
        }
    }
    return earliest;
}

void NetworkSessionManager::handleTimeout(Clock::time_point now)
{
    ++depth_;
    for (const auto &[id, entry] : sessions_) {
        if (const auto deadline = entry->session.nextTimeout(); deadline && *deadline <= now) {
            entry->session.handleTimeout(now);
        }
    }
    --depth_;
    reap();
}

void NetworkSessionManager::reap()
{
    if (depth_ != 0) {
        return;
    }
    const auto is_closed = [](const auto &session) {
        return session.second->closed.has_value();
    };
    for (auto entry = std::ranges::find_if(sessions_, is_closed); entry != sessions_.end();
         entry = std::ranges::find_if(sessions_, is_closed)) {
        const auto id = entry->first;
        const auto reason = *entry->second->closed;
        std::erase_if(routes_, [id](const auto &route) { return route.second == id; });
        ufrags_.erase(entry->second->ufrag);
        sessions_.erase(entry);
        ++depth_;
        listener_.onSessionClosed(id, reason);
        --depth_;
    }
}

}  // namespace endgate::nethernet
