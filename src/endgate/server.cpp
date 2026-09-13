#include "endgate/server.h"

#include <utility>

#include "endgate/logging/manager.h"

namespace endgate {

namespace {
constexpr auto config_path = "endgate.toml";
}  // namespace

ProxyServer::ProxyServer() : logger_(LogManager::getLogger()), console_(*this) {}

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
    return true;
}

void ProxyServer::shutdown(bool explicit_exit)
{
    bool in_progress = false;
    if (!shutdown_in_progress_.compare_exchange_strong(in_progress, true)) {
        return;
    }

    logger_.info("Shutting down the proxy...");
    {
        const std::scoped_lock lock(shutdown_mutex_);
        shutdown_.store(true);
    }
    shutdown_cv_.notify_all();
}

void ProxyServer::awaitProxyShutdown()
{
    std::unique_lock lock(shutdown_mutex_);
    shutdown_cv_.wait(lock, [this] { return shutdown_.load(); });
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
