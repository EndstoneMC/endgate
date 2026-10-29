#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

#include "endgate/network/rtc/description.h"
#include "endgate/network/rtc/ice.h"

namespace endgate {

class Endpoint;

struct PendingJoin {
    rtc::Description offer;
    rtc::IceCredentials credentials;
    std::string network_id;
    std::chrono::steady_clock::time_point created;
};

/**
 * The server-wide tables that endpoints on different loops share, each behind one mutex: the RakNet
 * client GUIDs in use, the NetherNet joins waiting for their first check, and the endpoint that owns
 * each NetherNet session.
 *
 * @see com.velocitypowered.proxy.VelocityServer#registerConnection
 */
class Registry {
public:
    using Claim = std::variant<std::monostate, PendingJoin, Endpoint *>;

    [[nodiscard]] bool addGuid(std::uint64_t guid)
    {
        const std::scoped_lock lock(mutex_);
        return guids_.insert(guid).second;
    }

    void removeGuid(std::uint64_t guid)
    {
        const std::scoped_lock lock(mutex_);
        guids_.erase(guid);
    }

    [[nodiscard]] std::uint64_t nextSessionId()
    {
        const std::scoped_lock lock(mutex_);
        return next_session_id_++;
    }

    [[nodiscard]] bool addPending(PendingJoin join)
    {
        const std::scoped_lock lock(mutex_);
        if (owners_.contains(join.credentials.ufrag)) {
            return false;
        }
        auto ufrag = join.credentials.ufrag;
        return pending_.try_emplace(std::move(ufrag), std::move(join)).second;
    }

    void removePending(const std::string &ufrag)
    {
        const std::scoped_lock lock(mutex_);
        pending_.erase(ufrag);
    }

    [[nodiscard]] Claim claim(const std::string &ufrag, Endpoint &claimant)
    {
        const std::scoped_lock lock(mutex_);
        if (const auto join = pending_.find(ufrag); join != pending_.end()) {
            Claim claimed{std::move(join->second)};
            pending_.erase(join);
            owners_.emplace(ufrag, &claimant);
            return claimed;
        }
        if (const auto owner = owners_.find(ufrag); owner != owners_.end()) {
            return owner->second;
        }
        return std::monostate{};
    }

    void release(const std::string &ufrag)
    {
        const std::scoped_lock lock(mutex_);
        owners_.erase(ufrag);
    }

private:
    std::mutex mutex_;
    std::unordered_set<std::uint64_t> guids_;
    std::unordered_map<std::string, PendingJoin> pending_;
    std::unordered_map<std::string, Endpoint *> owners_;
    std::uint64_t next_session_id_{1};
};

}  // namespace endgate
