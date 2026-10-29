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
    auto &logger = endgate::LogManager::getLogger("endgate::main");
    const auto start_time = std::chrono::steady_clock::now();

    endgate::ProxyServer server;
    if (!server.start()) {
        spdlog::shutdown();
        return 1;
    }

    const std::chrono::duration<double> startup_time = std::chrono::steady_clock::now() - start_time;
    logger.info("Done ({:.2f}s)!", startup_time.count());

    server.getConsoleCommandSource().start();
    server.awaitProxyShutdown();

    spdlog::shutdown();
    return 0;
}
