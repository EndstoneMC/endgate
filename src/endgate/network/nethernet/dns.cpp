#include "endgate/network/nethernet/dns.h"

#include <algorithm>
#include <utility>

namespace endgate::nethernet {

std::optional<std::vector<asio::ip::address>> DnsCache::tryGet(std::string_view hostname, Clock::time_point now)
{
    const auto entry = cache_.find(hostname);
    if (entry == cache_.end()) {
        return std::nullopt;
    }
    if (entry->second.expiration <= now) {
        cache_.erase(entry);
        return std::nullopt;
    }
    return entry->second.addresses;
}

void DnsCache::add(std::string_view hostname, std::vector<asio::ip::address> result, Clock::time_point now)
{
    if (config_.capacity == 0) {
        return;
    }
    if (const auto existing = cache_.find(hostname); existing != cache_.end()) {
        existing->second = {.addresses = std::move(result), .expiration = now + config_.lifetime};
        return;
    }
    std::erase_if(cache_, [now](const auto &entry) { return entry.second.expiration <= now; });
    if (cache_.size() >= config_.capacity) {
        cache_.erase(std::ranges::min_element(cache_, {}, [](const auto &entry) { return entry.second.expiration; }));
    }
    cache_.emplace(std::string(hostname), Entry{.addresses = std::move(result), .expiration = now + config_.lifetime});
}

}  // namespace endgate::nethernet
