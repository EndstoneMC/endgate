#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "endgate/network/rtc/sctp/association.h"
#include "endgate/network/rtc/sctp/transport.h"

namespace endgate::rtc::sctp {

struct Endpoint : Association::Listener {
    void sendPacket(std::span<const std::byte> packet) override { outbox.emplace_back(packet.begin(), packet.end()); }
    void onEstablished() override { ++established; }
    void onRestarted() override { ++restarted; }
    void onClosed(Association::CloseReason reason) override { closed = reason; }
    void onMessage(Message message) override
    {
        if (on_message) {
            on_message(message);
        }
        messages.push_back(std::move(message));
    }
    void onIncomingStreamsReset(std::span<const std::uint16_t> streams) override
    {
        incoming_resets.emplace_back(streams.begin(), streams.end());
    }
    void onOutgoingStreamsReset(std::span<const std::uint16_t> streams, bool performed) override
    {
        outgoing_resets.emplace_back(std::vector<std::uint16_t>(streams.begin(), streams.end()), performed);
    }

    std::vector<std::vector<std::byte>> outbox;
    unsigned established{0};
    unsigned restarted{0};
    std::optional<Association::CloseReason> closed;
    std::vector<Message> messages;
    std::vector<std::vector<std::uint16_t>> incoming_resets;
    std::vector<std::pair<std::vector<std::uint16_t>, bool>> outgoing_resets;
    std::function<void(const Message &)> on_message;
};

inline std::function<void(std::span<std::byte>)> seeded(std::uint32_t seed)
{
    return [engine = std::mt19937{seed}](std::span<std::byte> out) mutable {
        for (auto &byte : out) {
            byte = static_cast<std::byte>(engine());
        }
    };
}

inline std::string text(const LegacyByteBuf &buf)
{
    std::string result(buf.length(), '\0');
    buf.copyTo(std::as_writable_bytes(std::span{result}));
    return result;
}

inline LegacyByteBuf bufferOf(const std::string &value)
{
    return LegacyByteBuf::copyBuffer(std::as_bytes(std::span{value}));
}

struct TransportEndpoint : SctpTransport::Listener {
    void sendPacket(std::span<const std::byte> packet) override { outbox.emplace_back(packet.begin(), packet.end()); }
    void onStateChange(SctpTransport::State state) override { states.push_back(state); }
    void onChannelOpen(const DataChannel &channel) override { opened.push_back(channel); }
    void onChannelMessage(const DataChannel &channel, LegacyByteBuf message, bool binary) override
    {
        if (on_message) {
            on_message(channel, message);
        }
        messages.push_back({channel.stream, channel.label, text(message), binary});
    }
    void onChannelClosed(const DataChannel &channel) override { closed.push_back(channel); }

    struct Received {
        std::uint16_t stream;
        std::string label;
        std::string payload;
        bool binary;
    };

    std::vector<std::vector<std::byte>> outbox;
    std::vector<SctpTransport::State> states;
    std::vector<DataChannel> opened;
    std::vector<Received> messages;
    std::vector<DataChannel> closed;
    std::function<void(const DataChannel &, const LegacyByteBuf &)> on_message;
};

inline void seedRandom(Association::Config &config, std::uint32_t seed)
{
    if (!config.random) {
        config.random = seeded(seed);
    }
}

inline void seedRandom(SctpTransport::Config &config, std::uint32_t seed)
{
    seedRandom(config.association, seed);
}

template <typename Engine, typename Recorder>
class BasicNetwork {
public:
    using Clock = std::chrono::steady_clock;

    struct Impairment {
        double loss{0};
        double duplication{0};
        double reordering{0};
    };

    struct Flight {
        Clock::time_point arrival;
        bool to_b;
        std::vector<std::byte> packet;
    };

    explicit BasicNetwork(typename Engine::Config a_config = {}, typename Engine::Config b_config = {},
                          std::uint32_t seed = 1)
        : engine_(seed)
    {
        seedRandom(a_config, seed * 2 + 1);
        seedRandom(b_config, seed * 2 + 2);
        a.emplace(endpoint_a, std::move(a_config));
        b.emplace(endpoint_b, std::move(b_config));
    }

    void collect()
    {
        const auto take = [&](Recorder &from, bool to_b) {
            for (auto &packet : from.outbox) {
                ++sent;
                if (filter && !filter(packet, to_b)) {
                    continue;
                }
                if (chance(impairment.loss)) {
                    continue;
                }
                const auto copies = chance(impairment.duplication) ? 2 : 1;
                for (auto copy = 0; copy < copies; ++copy) {
                    auto delay = latency;
                    if (chance(impairment.reordering)) {
                        delay += std::chrono::milliseconds{std::uniform_int_distribution<int>{1, 30}(engine_)};
                    }
                    in_flight.push_back({now + delay, to_b, packet});
                }
            }
            from.outbox.clear();
        };
        take(endpoint_a, true);
        take(endpoint_b, false);
        std::ranges::stable_sort(in_flight, {}, &Flight::arrival);
    }

    bool step(Clock::time_point until)
    {
        collect();
        std::optional<Clock::time_point> next;
        const auto consider = [&](std::optional<Clock::time_point> deadline) {
            if (deadline && (!next || *deadline < *next)) {
                next = deadline;
            }
        };
        consider(in_flight.empty() ? std::nullopt : std::optional{in_flight.front().arrival});
        consider(a->nextTimeout());
        consider(b->nextTimeout());
        if (!next || *next > until) {
            now = std::max(now, until);
            return false;
        }
        now = std::max(now, *next);
        if (!in_flight.empty() && in_flight.front().arrival <= now) {
            auto flight = std::move(in_flight.front());
            in_flight.erase(in_flight.begin());
            (flight.to_b ? *b : *a).handlePacket(flight.packet, now);
        }
        if (a->nextTimeout() && *a->nextTimeout() <= now) {
            a->handleTimeout(now);
        }
        if (b->nextTimeout() && *b->nextTimeout() <= now) {
            b->handleTimeout(now);
        }
        collect();
        return true;
    }

    void run(Clock::duration duration)
    {
        const auto until = now + duration;
        while (step(until)) {
        }
    }

    bool runUntil(const std::function<bool()> &done, Clock::duration limit)
    {
        const auto until = now + limit;
        while (!done()) {
            if (!step(until)) {
                return done();
            }
        }
        return true;
    }

    Recorder endpoint_a;
    Recorder endpoint_b;
    std::optional<Engine> a;
    std::optional<Engine> b;
    Clock::time_point now{Clock::time_point{} + std::chrono::hours{1}};
    Clock::duration latency{std::chrono::milliseconds{20}};
    Impairment impairment;
    std::function<bool(std::span<const std::byte>, bool)> filter;
    std::vector<Flight> in_flight;
    std::size_t sent{0};

private:
    bool chance(double probability)
    {
        return probability > 0 && std::uniform_real_distribution<double>{0, 1}(engine_) < probability;
    }

    std::mt19937 engine_;
};

using Network = BasicNetwork<Association, Endpoint>;
using TransportNetwork = BasicNetwork<SctpTransport, TransportEndpoint>;

inline std::optional<std::uint8_t> firstChunk(std::span<const std::byte> packet)
{
    const auto parsed = Packet::parse(packet);
    if (!parsed || parsed->chunks.empty()) {
        return std::nullopt;
    }
    return parsed->chunks.front().type;
}

inline bool hasChunk(std::span<const std::byte> packet, ChunkType type)
{
    const auto parsed = Packet::parse(packet);
    return parsed && std::ranges::any_of(parsed->chunks, [&](const Chunk &chunk) {
               return chunk.type == static_cast<std::uint8_t>(type);
           });
}

}  // namespace endgate::rtc::sctp
