#pragma once

#include <ctime>
#include <memory>

#include <spdlog/pattern_formatter.h>

namespace endgate {

/**
 * Renders the level of a log message under log4j2's names.
 *
 * @see org.apache.logging.log4j.core.pattern.LevelPatternConverter
 */
class LevelFormatter final : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg &msg, const std::tm &tm, spdlog::memory_buf_t &dest) override;
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override;
};

/**
 * Renders the name of the thread a log message came from.
 *
 * @see org.apache.logging.log4j.core.pattern.ThreadNamePatternConverter
 */
class ThreadFormatter final : public spdlog::custom_flag_formatter {
public:
    void format(const spdlog::details::log_msg &msg, const std::tm &tm, spdlog::memory_buf_t &dest) override;
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override;
};

}  // namespace endgate
