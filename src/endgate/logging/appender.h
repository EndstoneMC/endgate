#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

#include <spdlog/common.h>
#include <spdlog/details/console_globals.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>

namespace endgate {

class Console;

/**
 * Writes log messages to the console, keeping them above the prompt while the console reader
 * is running.
 *
 * @see net.minecrell.terminalconsole.TerminalConsoleAppender
 */
class TerminalConsoleAppender final : public spdlog::sinks::base_sink<spdlog::details::console_mutex::mutex_t> {
public:
    explicit TerminalConsoleAppender(std::FILE *target_file, spdlog::color_mode mode = spdlog::color_mode::automatic);

    void setColorMode(spdlog::color_mode mode);
    static void setReader(Console *console);

protected:
    void sink_it_(const spdlog::details::log_msg &msg) override;
    void flush_() override;

private:
    std::FILE *target_file_;
    bool should_do_colors_;
    std::array<std::string, spdlog::level::n_levels> colors_;
    std::array<spdlog::pattern_formatter, 2> formatters_;
};

/**
 * Writes log messages to the latest log file, rolling the previous one into a dated gzip
 * archive daily and on startup.
 *
 * @see org.apache.logging.log4j.core.appender.RollingRandomAccessFileAppender
 */
class RollingRandomAccessFileAppender final : public spdlog::sinks::base_sink<std::mutex> {
public:
    RollingRandomAccessFileAppender(std::filesystem::path file_name, std::string file_pattern, std::uint16_t max_files);

protected:
    void sink_it_(const spdlog::details::log_msg &msg) override;
    void flush_() override;

private:
    [[nodiscard]] std::filesystem::path calcFilename(std::size_t index) const;
    void rotate();

    std::filesystem::path file_name_;
    std::string file_pattern_;
    std::uint16_t max_files_;
    spdlog::log_clock::time_point rotation_tp_;
    spdlog::details::file_helper file_helper_;
    std::array<spdlog::pattern_formatter, 2> formatters_;
};

}  // namespace endgate
