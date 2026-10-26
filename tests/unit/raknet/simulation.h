#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <random>
#include <span>
#include <vector>

#include "endgate/network/raknet/protocol.h"

namespace endgate::raknet::test {

struct LinkConditions {
    double loss{0};
    double duplicate{0};
    double reorder{0};
    std::chrono::microseconds latency{500};
    std::chrono::microseconds reorder_delay{3000};
};

class Simulation {
public:
    struct Node {
        std::function<void(std::span<const std::byte>, TimePoint)> receive;
        std::function<std::optional<TimePoint>()> next_timeout;
        std::function<void(TimePoint)> timeout;
    };

    explicit Simulation(std::uint32_t seed = 1) : rng_(seed) {}

    [[nodiscard]] TimePoint now() const { return now_; }
    void advance(std::chrono::microseconds by) { now_ += by; }

    void attach(std::size_t index, Node node) { nodes_.at(index) = std::move(node); }
    void setConditions(std::size_t from, LinkConditions conditions) { conditions_.at(from) = conditions; }

    void transmit(std::size_t from, std::span<const std::byte> datagram)
    {
        ++sent_.at(from);
        largest_.at(from) = std::max(largest_.at(from), datagram.size());
        const auto &conditions = conditions_.at(from);
        std::uniform_real_distribution<double> chance{0, 1};
        if (chance(rng_) < conditions.loss) {
            return;
        }
        const auto copies = chance(rng_) < conditions.duplicate ? 2 : 1;
        for (int copy = 0; copy < copies; ++copy) {
            auto at = now_ + conditions.latency;
            if (chance(rng_) < conditions.reorder) {
                at += conditions.reorder_delay;
            }
            in_flight_.push({at, order_++, 1 - from, {datagram.begin(), datagram.end()}});
        }
    }

    bool runUntil(const std::function<bool()> &done, std::chrono::milliseconds limit)
    {
        const auto deadline = now_ + limit;
        for (std::size_t steps = 0; steps < 50'000'000; ++steps) {
            if (done()) {
                return true;
            }
            std::optional<TimePoint> next;
            if (!in_flight_.empty()) {
                next = in_flight_.top().at;
            }
            for (const auto &node : nodes_) {
                if (const auto timeout = node.next_timeout ? node.next_timeout() : std::nullopt;
                    timeout && (!next || *timeout < *next)) {
                    next = timeout;
                }
            }
            if (!next) {
                return done();
            }
            if (*next > deadline) {
                now_ = deadline;
                return done();
            }
            now_ = std::max(now_, *next);
            if (!in_flight_.empty() && in_flight_.top().at <= now_) {
                auto datagram = in_flight_.top();
                in_flight_.pop();
                ++delivered_.at(datagram.to);
                nodes_.at(datagram.to).receive(datagram.bytes, now_);
                continue;
            }
            for (auto &node : nodes_) {
                if (const auto timeout = node.next_timeout ? node.next_timeout() : std::nullopt;
                    timeout && *timeout <= now_) {
                    node.timeout(now_);
                }
            }
        }
        return done();
    }

    [[nodiscard]] std::size_t sent(std::size_t from) const { return sent_.at(from); }
    [[nodiscard]] std::size_t delivered(std::size_t to) const { return delivered_.at(to); }
    [[nodiscard]] std::size_t largest(std::size_t from) const { return largest_.at(from); }

private:
    struct InFlight {
        TimePoint at;
        std::uint64_t order;
        std::size_t to;
        std::vector<std::byte> bytes;

        bool operator>(const InFlight &other) const { return at != other.at ? at > other.at : order > other.order; }
    };

    std::mt19937 rng_;
    TimePoint now_{std::chrono::hours{1}};
    std::array<Node, 2> nodes_;
    std::array<LinkConditions, 2> conditions_;
    std::priority_queue<InFlight, std::vector<InFlight>, std::greater<>> in_flight_;
    std::uint64_t order_{0};
    std::array<std::size_t, 2> sent_{};
    std::array<std::size_t, 2> delivered_{};
    std::array<std::size_t, 2> largest_{};
};

}  // namespace endgate::raknet::test
