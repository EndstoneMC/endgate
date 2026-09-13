#pragma once

#include <string>

#include <spdlog/logger.h>

namespace endgate {

/**
 * Hands out loggers wired up to the proxy's appenders.
 *
 * @see org.apache.logging.log4j.LogManager
 */
class LogManager {
public:
    static spdlog::logger &getLogger(const std::string &name = {});
};

}  // namespace endgate
