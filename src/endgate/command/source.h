#pragma once

#include <concepts>
#include <string_view>
#include <typeinfo>

#include "endgate/permission/subject.h"

namespace endgate {

/**
 * Represents something that can be used to run a command.
 *
 * @see com.velocitypowered.api.command.CommandSource
 */
class CommandSource : public PermissionSubject {
public:
    /**
     * Sends a message to the source.
     *
     * @see net.kyori.adventure.audience.Audience#sendMessage
     */
    virtual void sendMessage(std::string_view message) = 0;

    /**
     * Whether this source is an instance of the given type. A subclass overrides it to answer for itself and
     * defer to its base.
     *
     * @see endstone::Object#isInstanceOf
     */
    [[nodiscard]] virtual bool isInstanceOf(const std::type_info &target) const
    {
        return target == typeid(CommandSource);
    }

    /**
     * Attempts to cast this source to the given type T, and returns nullptr if it is not an instance of T or a
     * subclass of T.
     *
     * @see endstone::Object#as
     */
    template <std::derived_from<CommandSource> T>
    [[nodiscard]] T *as()
    {
        return is<T>() ? static_cast<T *>(this) : nullptr;
    }

    template <std::derived_from<CommandSource> T>
    [[nodiscard]] const T *as() const
    {
        return is<T>() ? static_cast<const T *>(this) : nullptr;
    }

    /**
     * Checks whether this source is an instance of the given type T or a subclass of T, as Java's instanceof does.
     *
     * @see endstone::Object#is
     */
    template <std::derived_from<CommandSource> T>
    [[nodiscard]] bool is() const
    {
        return isInstanceOf(typeid(T));
    }
};

}  // namespace endgate
