#include "endgate/logging/manager.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <cxxabi.h>
#endif

#include <spdlog/cfg/env.h>
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
        spdlog::cfg::load_env_levels();
        return appenders;
    }();

    const std::scoped_lock lock(mutex);
    if (const auto existing = spdlog::get(name)) {
        return *existing;
    }

    auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    spdlog::initialize_logger(logger);
    return *logger;
}

spdlog::logger &LogManager::getLogger(const std::type_info &type)
{
#ifdef _WIN32
    std::string_view name = type.name();
    for (const std::string_view prefix : {"class ", "struct "}) {
        if (name.starts_with(prefix)) {
            name.remove_prefix(prefix.size());
        }
    }
#else
    auto status = 0;
    const std::unique_ptr<char, decltype([](char *p) { std::free(p); })> demangled(
        abi::__cxa_demangle(type.name(), nullptr, nullptr, &status));
    std::string_view name = status == 0 ? demangled.get() : type.name();
#endif
    name = name.substr(0, name.find('<'));
    if (const auto scope = name.rfind("::"); scope != std::string_view::npos) {
        name.remove_prefix(scope + 2);
    }
    return getLogger(std::string{name});
}

}  // namespace endgate
