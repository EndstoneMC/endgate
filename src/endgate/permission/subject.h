#pragma once

#include <string>

#include "endgate/permission/tristate.h"

namespace endgate {

/**
 * Represents an object that has a set of queryable permissions.
 *
 * @see com.velocitypowered.api.permission.PermissionSubject
 */
class PermissionSubject {
public:
    virtual ~PermissionSubject() = default;

    /**
     * Determines whether the subject has a particular permission.
     *
     * @see com.velocitypowered.api.permission.PermissionSubject#hasPermission
     */
    [[nodiscard]] bool hasPermission(const std::string &permission) const
    {
        return asBoolean(getPermissionValue(permission));
    }

    /**
     * Gets the subject state on a particular permission.
     *
     * @see com.velocitypowered.api.permission.PermissionSubject#getPermissionValue
     */
    [[nodiscard]] virtual Tristate getPermissionValue(const std::string &permission) const = 0;
};

}  // namespace endgate
