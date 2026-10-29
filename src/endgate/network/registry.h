#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

#include "endgate/buffer/bytebuf.h"
#include "endgate/network/rtc/description.h"
#include "endgate/network/rtc/ice.h"
#include "endgate/network/rtc/stun.h"

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
 * client GUIDs in use, the NetherNet joins waiting for their first check, the endpoint that owns each
 * NetherNet session, and how many RakNet handshakes are half open.
 *
 * @see com.velocitypowered.proxy.VelocityServer#registerConnection
 */
class Registry {
public:
    enum class AddPending {
        Added,
        Collision,
        Full,
    };

    using Claim = std::variant<std::monostate, PendingJoin, Endpoint *>;

    explicit Registry(std::size_t half_open_limit = 1024, std::size_t pending_limit = 1024)
        : half_open_limit_(half_open_limit), pending_limit_(pending_limit)
    {
    }

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

    [[nodiscard]] bool addHalfOpen()
    {
        const std::scoped_lock lock(mutex_);
        if (half_open_ >= half_open_limit_) {
            return false;
        }
        ++half_open_;
        return true;
    }

    void removeHalfOpen()
    {
        const std::scoped_lock lock(mutex_);
        if (half_open_ > 0) {
            --half_open_;
        }
    }

    [[nodiscard]] std::uint64_t nextSessionId()
    {
        const std::scoped_lock lock(mutex_);
        return next_session_id_++;
    }

    [[nodiscard]] AddPending addPending(const PendingJoin &join)
    {
        const std::scoped_lock lock(mutex_);
        if (owners_.contains(join.credentials.ufrag) || pending_.contains(join.credentials.ufrag)) {
            return AddPending::Collision;
        }
        if (pending_.size() >= pending_limit_) {
            return AddPending::Full;
        }
        pending_.emplace(join.credentials.ufrag, join);
        return AddPending::Added;
    }

    void removePending(const std::string &ufrag)
    {
        const std::scoped_lock lock(mutex_);
        pending_.erase(ufrag);
    }

    [[nodiscard]] Claim claim(const std::string &ufrag, Endpoint &claimant, const rtc::StunMessage &check,
                              ByteBufView datagram)
    {
        const std::scoped_lock lock(mutex_);
        if (const auto join = pending_.find(ufrag); join != pending_.end()) {
            if (check.message_class != rtc::StunClass::Request ||
                check.username != ufrag + ":" + join->second.offer.ice_ufrag ||
                !check.verifyIntegrity(datagram, join->second.credentials.pwd)) {
                return std::monostate{};
            }
            Claim claimed{std::move(join->second)};
            owners_.emplace(ufrag, Owner{.endpoint = &claimant,
                                         .username = *check.username,
                                         .pwd = std::get<PendingJoin>(claimed).credentials.pwd});
            pending_.erase(join);
            return claimed;
        }
        if (const auto owner = owners_.find(ufrag); owner != owners_.end()) {
            if (check.message_class != rtc::StunClass::Request || check.username != owner->second.username ||
                !check.verifyIntegrity(datagram, owner->second.pwd)) {
                return std::monostate{};
            }
            return owner->second.endpoint;
        }
        return std::monostate{};
    }

    void release(const std::string &ufrag)
    {
        const std::scoped_lock lock(mutex_);
        owners_.erase(ufrag);
    }

private:
    struct Owner {
        Endpoint *endpoint;
        std::string username;
        std::string pwd;
    };

    std::mutex mutex_;
    std::size_t half_open_limit_;
    std::size_t pending_limit_;
    std::size_t half_open_{0};
    std::unordered_set<std::uint64_t> guids_;
    std::unordered_map<std::string, PendingJoin> pending_;
    std::unordered_map<std::string, Owner> owners_;
    std::uint64_t next_session_id_{1};
};

}  // namespace endgate
