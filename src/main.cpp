#include <chrono>

#include <spdlog/spdlog.h>

#include "endgate/logging/manager.h"
#include "endgate/server.h"

/**
 * Launches the proxy.
 *
 * @see com.velocitypowered.proxy.Velocity
 */
int main()
{
    const auto start_time = std::chrono::steady_clock::now();

    endgate::ProxyServer server;
    server.start();

    const std::chrono::duration<double> boot_time = std::chrono::steady_clock::now() - start_time;
    endgate::LogManager::getLogger().info("Done ({:.2f}s)!", boot_time.count());

    server.getConsoleCommandSource().start();
    server.awaitProxyShutdown();

    spdlog::shutdown();
    return 0;
}
