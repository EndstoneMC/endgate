#pragma once

#include <optional>
#include <replxx.hxx>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

namespace endgate {

class ProxyServer;

/**
 * Implements the console, including running commands and receiving messages from plugins.
 *
 * @see com.velocitypowered.proxy.console.VelocityConsole
 */
class Console {
public:
    explicit Console(ProxyServer &server);
    ~Console();

    void start();
    void sendMessage(std::string_view message);
    void printAbove(std::string_view message);

private:
    [[nodiscard]] bool isRunning() const;
    void runCommand(const std::string &command);
    void shutdown();

    ProxyServer &server_;
    spdlog::logger &logger_;
    std::optional<replxx::Replxx> rx_;
};

}  // namespace endgate
