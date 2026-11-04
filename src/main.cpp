#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "endgate/logging/manager.h"
#include "endgate/server.h"
#include "endgate/thread.h"

/**
 * Launches the proxy.
 *
 * @see com.velocitypowered.proxy.Velocity
 */
int main()
{
#ifdef _WIN32
    for (const DWORD handle_id : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        HANDLE handle = GetStdHandle(handle_id);
        DWORD mode = 0;
        if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) != 0) {
            SetConsoleMode(handle, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#endif
    endgate::Thread::setName("main");
    auto &logger = endgate::LogManager::getLogger("main");
    const auto start_time = std::chrono::steady_clock::now();

    endgate::ProxyServer server;
    if (!server.start()) {
        return 1;
    }

    const std::chrono::duration<double> startup_time = std::chrono::steady_clock::now() - start_time;
    logger.info("Done ({:.2f}s)!", startup_time.count());

    server.getConsoleCommandSource().start();
    server.awaitProxyShutdown();
    return 0;
}
