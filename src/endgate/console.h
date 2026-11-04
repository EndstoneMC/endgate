#pragma once

#include <memory>
#include <optional>
#include <replxx.hxx>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "endgate/command/source.h"

namespace endgate {

class ProxyServer;

/**
 * Implements the console, including running commands and receiving messages from plugins.
 *
 * @see com.velocitypowered.proxy.console.VelocityConsole
 */
class Console final : public CommandSource, public std::enable_shared_from_this<Console> {
public:
    explicit Console(ProxyServer &server);
    ~Console();

    void start();
    void sendMessage(std::string_view message) override;

    /**
     * @see endstone::core::EndstoneConsoleCommandSender#sendErrorMessage
     */
    void sendErrorMessage(std::string_view message) override;

    /**
     * The console has every permission until a permission provider exists.
     *
     * @see com.velocitypowered.proxy.console.VelocityConsole#getPermissionValue
     */
    [[nodiscard]] Tristate getPermissionValue(const std::string &) const override { return Tristate::True; }

    [[nodiscard]] bool isInstanceOf(const std::type_info &target) const override
    {
        return target == typeid(Console) || CommandSource::isInstanceOf(target);
    }
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
