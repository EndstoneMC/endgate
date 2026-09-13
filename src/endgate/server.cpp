#include "endgate/server.h"

#include <charconv>
#include <utility>

#include <asio/ip/address.hpp>

#include "endgate/logging/manager.h"

namespace endgate {

namespace {
constexpr auto config_path = "endgate.toml";
}  // namespace

ProxyServer::ProxyServer() : logger_(LogManager::getLogger()), cm_(*this), console_(*this) {}

bool ProxyServer::start()
{
    const auto version = getVersion();
    logger_.info("Booting up {} {}...", version.getName(), version.getVersion());

    auto configuration = ProxyConfig::read(config_path);
    if (!configuration) {
        logger_.error("Unable to read the configuration: {}", configuration.error());
        return false;
    }
    configuration_ = std::move(*configuration);

    cm_.logChannelInformation();

    const auto &bind = configuration_.getBind();
    const auto colon = bind.rfind(':');
    if (colon == std::string::npos) {
        logger_.error("Invalid bind address: {}", bind);
        return false;
    }

    asio::error_code ec;
    const auto address = asio::ip::make_address(bind.substr(0, colon), ec);
    const auto digits = bind.substr(colon + 1);
    std::uint16_t port{};
    if (ec || std::from_chars(digits.data(), digits.data() + digits.size(), port).ec != std::errc{}) {
        logger_.error("Invalid bind address: {}", bind);
        return false;
    }

    if (!cm_.bind(address, port)) {
        return false;
    }
    cm_.lanDiscoveryBind(address);

    cm_.start();
    return true;
}

void ProxyServer::shutdown(bool explicit_exit)
{
    bool in_progress = false;
    if (!shutdown_in_progress_.compare_exchange_strong(in_progress, true)) {
        return;
    }

    logger_.info("Shutting down the proxy...");
    cm_.shutdown();
    shutdown_.store(true);
}

void ProxyServer::awaitProxyShutdown()
{
    cm_.awaitTermination();
}

bool ProxyServer::isShutdown() const
{
    return shutdown_.load();
}

bool ProxyServer::isShuttingDown() const
{
    return shutdown_in_progress_.load();
}

ProxyVersion ProxyServer::getVersion() const
{
    return {"endgate", "EndstoneMC", ENDGATE_VERSION};
}

Console &ProxyServer::getConsoleCommandSource()
{
    return console_;
}

const ProxyConfig &ProxyServer::getConfiguration() const
{
    return configuration_;
}

}  // namespace endgate
