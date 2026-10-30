#pragma once

#include <string>
#include <type_traits>
#include <typeinfo>

#include <spdlog/logger.h>

namespace endgate {

/**
 * Hands out loggers wired up to the proxy's appenders.
 *
 * @see org.apache.logging.log4j.LogManager
 */
class LogManager {
public:
    static spdlog::logger &getLogger(const std::string &name);

    /**
     * Returns a Logger named after the class of the object, without its namespace or template arguments.
     *
     * @see org.apache.logging.log4j.LogManager#getLogger(Class)
     */
    template <typename T>
        requires std::is_class_v<T>
    static spdlog::logger &getLogger(const T &object)
    {
        return getLogger(typeid(object));
    }

private:
    static spdlog::logger &getLogger(const std::type_info &type);
};

}  // namespace endgate
