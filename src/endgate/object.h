#pragma once

#include <concepts>
#include <typeinfo>

namespace endgate {

/**
 * Base class of every hierarchy narrowed at runtime, as Java's instanceof and cast narrow a type.
 *
 * @see endstone::Object
 */
class Object {
public:
    virtual ~Object() = default;

    /**
     * Whether this object is an instance of the given type. A subclass overrides it to answer for itself and
     * defer to its base.
     *
     * @see endstone::Object#isInstanceOf
     */
    [[nodiscard]] virtual bool isInstanceOf(const std::type_info &target) const { return target == typeid(Object); }

    /**
     * Attempts to cast this object to the given type T, and returns nullptr if it is not an instance of T or a
     * subclass of T.
     *
     * @see endstone::Object#as
     */
    template <std::derived_from<Object> T>
    [[nodiscard]] T *as()
    {
        return is<T>() ? static_cast<T *>(this) : nullptr;
    }

    template <std::derived_from<Object> T>
    [[nodiscard]] const T *as() const
    {
        return is<T>() ? static_cast<const T *>(this) : nullptr;
    }

    /**
     * Checks whether this object is an instance of the given type T or a subclass of T, as Java's instanceof does.
     *
     * @see endstone::Object#is
     */
    template <std::derived_from<Object> T>
    [[nodiscard]] bool is() const
    {
        return isInstanceOf(typeid(T));
    }
};

}  // namespace endgate
