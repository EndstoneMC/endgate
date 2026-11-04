#pragma once

#include <ctime>
#include <memory>
#include <string>
#include <string_view>

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

/**
 * Renders the message of a log message, turning each section sign code into an ANSI escape sequence or
 * removing it.
 *
 * @see net.minecrell.terminalconsole.MinecraftFormattingConverter
 */
class MinecraftFormattingConverter final : public spdlog::custom_flag_formatter {
public:
    explicit MinecraftFormattingConverter(bool strip) : strip_(strip) {}

    void format(const spdlog::details::log_msg &msg, const std::tm &tm, spdlog::memory_buf_t &dest) override;
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override;

    [[nodiscard]] static std::string convert(std::string_view message, bool strip);

private:
    bool strip_;
};

}  // namespace endgate
