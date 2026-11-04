#pragma once

#include <format>
#include <string>
#include <string_view>

namespace endgate {

/**
 * Basic, common command messages.
 *
 * @see com.velocitypowered.proxy.command.builtin.CommandMessages
 */
struct CommandMessages {
    static constexpr std::string_view PLAYERS_ONLY = "Only players can run this command.";

    [[nodiscard]] static std::string serverDoesNotExist(std::string_view name)
    {
        return std::format("The specified server {} does not exist.", name);
    }

    [[nodiscard]] static std::string playerNotFound(std::string_view name)
    {
        return std::format("The specified player {} does not exist.", name);
    }
};

}  // namespace endgate
