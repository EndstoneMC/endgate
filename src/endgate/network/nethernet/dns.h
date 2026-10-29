#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <asio/ip/address.hpp>

namespace endgate::nethernet {

/**
 * Remembers what a hostname resolved to until the answer expires, holding at most a fixed number of
 * hostnames.
 *
 * @see NetherNet::DnsCache
 */
class DnsCache {
public:
    using Clock = std::chrono::steady_clock;

    struct Config {
        std::chrono::seconds lifetime{std::chrono::minutes{5}};
        std::size_t capacity{256};
    };

    DnsCache() = default;
    explicit DnsCache(Config config) : config_(config) {}

    /**
     * @see NetherNet::DnsCache::TryGet
     */
    [[nodiscard]] std::optional<std::vector<asio::ip::address>> tryGet(std::string_view hostname,
                                                                       Clock::time_point now);

    /**
     * @see NetherNet::DnsCache::Add
     */
    void add(std::string_view hostname, std::vector<asio::ip::address> result, Clock::time_point now);

    [[nodiscard]] std::size_t size() const { return cache_.size(); }

private:
    struct Entry {
        std::vector<asio::ip::address> addresses;
        Clock::time_point expiration;
    };

    Config config_;
    std::map<std::string, Entry, std::less<>> cache_;
};

}  // namespace endgate::nethernet
