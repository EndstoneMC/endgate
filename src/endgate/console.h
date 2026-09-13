#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <replxx.hxx>

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
    std::optional<replxx::Replxx> rx_;
};

}  // namespace endgate
