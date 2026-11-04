#include "endgate/logging/formatter.h"

#include <string>
#include <string_view>

#include <spdlog/details/fmt_helper.h>

#include "endgate/thread.h"

namespace endgate {

void LevelFormatter::format(const spdlog::details::log_msg &msg, const std::tm &, spdlog::memory_buf_t &dest)
{
    std::string_view name;
    switch (msg.level) {
    case spdlog::level::trace:
        name = "TRACE";
        break;
    case spdlog::level::debug:
        name = "DEBUG";
        break;
    case spdlog::level::info:
        name = "INFO";
        break;
    case spdlog::level::warn:
        name = "WARN";
        break;
    case spdlog::level::err:
        name = "ERROR";
        break;
    case spdlog::level::critical:
        name = "FATAL";
        break;
    default:
        name = "OFF";
        break;
    }
    spdlog::details::fmt_helper::append_string_view(name, dest);
}

std::unique_ptr<spdlog::custom_flag_formatter> LevelFormatter::clone() const
{
    return std::make_unique<LevelFormatter>();
}

void ThreadFormatter::format(const spdlog::details::log_msg &msg, const std::tm &, spdlog::memory_buf_t &dest)
{
    if (const auto name = Thread::getName(); !name.empty()) {
        spdlog::details::fmt_helper::append_string_view(name, dest);
        return;
    }
    spdlog::details::fmt_helper::append_int(msg.thread_id, dest);
}

std::unique_ptr<spdlog::custom_flag_formatter> ThreadFormatter::clone() const
{
    return std::make_unique<ThreadFormatter>();
}

}  // namespace endgate
