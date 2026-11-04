#pragma once

#include <string_view>
#include <typeinfo>

#include "endgate/object.h"
#include "endgate/permission/subject.h"

namespace endgate {

/**
 * Represents something that can be used to run a command.
 *
 * @see com.velocitypowered.api.command.CommandSource
 */
class CommandSource : public PermissionSubject, public Object {
public:
    /**
     * Sends a message to the source.
     *
     * @see net.kyori.adventure.audience.Audience#sendMessage
     */
    virtual void sendMessage(std::string_view message) = 0;

    /**
     * Sends an error message to the source.
     *
     * @see endstone::CommandSender#sendErrorMessage
     */
    virtual void sendErrorMessage(std::string_view message) = 0;

    [[nodiscard]] bool isInstanceOf(const std::type_info &target) const override
    {
        return target == typeid(CommandSource) || Object::isInstanceOf(target);
    }
};

}  // namespace endgate
