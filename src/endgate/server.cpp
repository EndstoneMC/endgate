#include "endgate/server.h"

#include "endgate/logging/manager.h"

namespace endgate {

ProxyServer::ProxyServer() : console_(*this) {}

void ProxyServer::start()
{
    const auto version = getVersion();
    LogManager::getLogger().info("Booting up {} {}...", version.getName(), version.getVersion());
}

void ProxyServer::shutdown(bool explicit_exit)
{
    bool in_progress = false;
    if (!shutdown_in_progress_.compare_exchange_strong(in_progress, true)) {
        return;
    }

    LogManager::getLogger().info("Shutting down the proxy...");
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

}  // namespace endgate
