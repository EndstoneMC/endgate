#include "endgate/network/ping.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include "endgate/backend.h"
#include "endgate/config.h"
#include "endgate/connection/login.h"
#include "endgate/logging/manager.h"
#include "endgate/network/version.h"
#include "endgate/server.h"

namespace endgate {

ServerListPingHandler::ServerListPingHandler(ProxyServer &server)
    : server_(server), logger_(LogManager::getLogger(*this))
{
}

ServerPing ServerListPingHandler::constructLocalPing(std::optional<int> protocol) const
{
    const auto &configuration = server_.getConfiguration();
    const auto shown = protocol.and_then(getProtocolVersion).value_or(MAXIMUM_VERSION);
    return {ServerPing::Version{shown.getProtocol(), std::string{shown.getName()}},
            ServerPing::Players{0, configuration.getShowMaxPlayers()}, configuration.getMotd(),
            ProxyServer::getVersion().getName(), GameType::Survival};
}

void ServerListPingHandler::activated(std::string_view association, std::optional<int> protocol) const
{
    if (!server_.getConfiguration().isShowPingRequests()) {
        return;
    }
    logger_.info("[initial connection] {} is pinging the server with version {}",
                 server_.getConfiguration().isPlayerAddressLoggingEnabled() ? association : "<ip address withheld>",
                 protocol ? describeProtocol(*protocol) : "unknown");
}

Awaitable<ServerPing> ServerListPingHandler::attemptPingPassthrough(PingPassthroughMode mode,
                                                                    std::optional<int> protocol,
                                                                    const std::vector<std::string> &servers,
                                                                    asio::io_context &io_context) const
{
    auto fallback = constructLocalPing(protocol);
    struct Pending {
        explicit Pending(const asio::any_io_executor &executor) : done(executor, asio::steady_timer::time_point::max())
        {
        }

        std::vector<std::optional<ServerPing>> responses;
        std::size_t remaining = 0;
        asio::steady_timer done;
    };

    std::vector<BackendServer *> backends;
    for (const auto &name : servers) {
        auto *const backend = server_.getServer(name);
        if (backend != nullptr && !(protocol && backend->isOnOtherProtocol(*protocol))) {
            backends.push_back(backend);
        }
    }
    if (backends.empty()) {
        co_return fallback;
    }

    const auto executor = co_await asio::this_coro::executor;
    const auto pending = std::make_shared<Pending>(executor);
    pending->responses.resize(backends.size());
    pending->remaining = backends.size();
    for (std::size_t i = 0; i < backends.size(); ++i) {
        asio::co_spawn(
            executor,
            [](BackendServer *backend, asio::io_context &io, std::shared_ptr<Pending> state,
               std::size_t index) -> Awaitable<void> {
                try {
                    state->responses[index] = co_await backend->ping(io);
                }
                catch (const std::exception &) {
                    state->responses[index].reset();
                }
                if (--state->remaining == 0) {
                    state->done.cancel();
                }
            }(backends[i], io_context, pending, i),
            asio::detached);
    }
    if (pending->remaining != 0) {
        co_await pending->done.async_wait(asio::as_tuple(asio::use_awaitable));
    }

    for (const auto &response : pending->responses) {
        if (!response || (protocol && response->getVersion().getProtocol() != *protocol)) {
            continue;
        }
        if (mode == PingPassthroughMode::All) {
            co_return *response;
        }
        co_return ServerPing{fallback.getVersion(), fallback.getPlayers(), response->getDescription(),
                             response->getLevelName(), fallback.getGameType()};
    }
    co_return fallback;
}

Awaitable<ServerPing> ServerListPingHandler::getInitialPing(std::optional<int> protocol, std::string_view virtual_host,
                                                            asio::io_context &io_context) const
{
    const auto &configuration = server_.getConfiguration();
    const auto mode = configuration.getPingPassthrough();
    if (mode == PingPassthroughMode::Disabled) {
        co_return constructLocalPing(protocol);
    }
    const auto &forced_hosts = configuration.getForcedHosts();
    const auto forced = forced_hosts.find(InitialLoginSessionHandler::cleanVhost(virtual_host));
    const auto &servers = forced != forced_hosts.end() && !forced->second.empty()
                            ? forced->second
                            : configuration.getAttemptConnectionOrder();
    co_return co_await attemptPingPassthrough(mode, protocol, servers, io_context);
}

std::string ServerPing::toMotd(std::uint64_t guid, std::uint16_t port) const
{
    constexpr int joinable_through_server_screen = 1;
    constexpr int editor_world = 0;

    return std::format("MCPE;{};{};{};{};{};{};{};{};{};{};{};{};", description_, version_.getProtocol(),
                       version_.getName(), players_.getOnline(), players_.getMax(), guid, level_name_,
                       toString(game_type_), joinable_through_server_screen, port, port, editor_world);
}

std::optional<ServerPing> ServerPing::parseMotd(std::string_view announcement)
{
    constexpr std::size_t protocol = 2;
    constexpr std::size_t level = 7;

    std::vector<std::string_view> fields;
    for (const auto field : std::views::split(announcement, ';')) {
        fields.emplace_back(field);
    }
    if (fields.size() <= protocol || fields.front() != "MCPE") {
        return std::nullopt;
    }
    const auto number = [&fields](std::size_t index) -> std::optional<int> {
        if (index >= fields.size()) {
            return std::nullopt;
        }
        const auto field = fields[index];
        int value = 0;
        const auto read = std::from_chars(field.data(), field.data() + field.size(), value);
        if (read.ec != std::errc{} || read.ptr != field.data() + field.size()) {
            return std::nullopt;
        }
        return value;
    };
    const auto version = number(protocol);
    if (!version || *version <= 0) {
        return std::nullopt;
    }
    return ServerPing{Version{*version, std::string{fields.size() > 3 ? fields[3] : ""}},
                      Players{number(4).value_or(0), number(5).value_or(0)},
                      std::string{fields.size() > 1 ? fields[1] : ""},
                      std::string{fields.size() > level ? fields[level] : ""},
                      fromString(fields.size() > 8 ? fields[8] : "").value_or(GameType::Survival)};
}

}  // namespace endgate
