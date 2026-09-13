#include "endgate/logging/manager.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

#include <spdlog/spdlog.h>

#include "endgate/logging/appender.h"

namespace endgate {

spdlog::logger &LogManager::getLogger(const std::string &name)
{
    static std::mutex mutex;
    static const std::vector<spdlog::sink_ptr> sinks = [] {
        std::vector<spdlog::sink_ptr> appenders{
            std::make_shared<TerminalConsoleAppender>(stdout),
            std::make_shared<RollingRandomAccessFileAppender>("logs/latest.log", "logs/{:%Y-%m-%d}-{}.log.gz", 7)};
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("", appenders.begin(), appenders.end()));
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::err);
        spdlog::flush_every(std::chrono::seconds(3));
        return appenders;
    }();

    if (name.empty()) {
        return *spdlog::default_logger();
    }

    const std::scoped_lock lock(mutex);
    if (const auto existing = spdlog::get(name)) {
        return *existing;
    }

    auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    spdlog::initialize_logger(logger);
    return *logger;
}

}  // namespace endgate
