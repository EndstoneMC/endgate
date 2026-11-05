#include "endgate/logging/appender.h"

#include <libdeflate.h>

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <utility>

#include <spdlog/details/os.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "endgate/console.h"
#include "endgate/logging/formatter.h"

namespace endgate {

namespace {

std::atomic<Console *> Reader{nullptr};

std::tm localtime(spdlog::log_clock::time_point tp)
{
    const std::time_t time = spdlog::log_clock::to_time_t(tp);
    return spdlog::details::os::localtime(time);
}

spdlog::log_clock::time_point nextRotation()
{
    const auto now = spdlog::log_clock::now();
    std::tm midnight = localtime(now);
    midnight.tm_hour = 0;
    midnight.tm_min = 0;
    midnight.tm_sec = 0;

    const auto rotation = spdlog::log_clock::from_time_t(std::mktime(&midnight));
    return rotation > now ? rotation : rotation + std::chrono::hours(24);
}

bool rename(const std::filesystem::path &src, const std::filesystem::path &target)
{
    std::error_code ec;
    std::filesystem::remove(target, ec);
    std::filesystem::rename(src, target, ec);
    return !ec;
}

bool compress(const std::filesystem::path &src, const std::filesystem::path &target)
{
    constexpr int compression_level = 6;

    std::error_code ec;
    std::filesystem::remove(target, ec);

    std::ifstream input(src, std::ios::binary | std::ios::ate);
    if (!input) {
        return false;
    }

    const auto size = static_cast<std::size_t>(input.tellg());
    std::string data(size, '\0');
    input.seekg(0);
    if (size > 0 && !input.read(data.data(), static_cast<std::streamsize>(size))) {
        return false;
    }
    input.close();

    auto *compressor = libdeflate_alloc_compressor(compression_level);
    if (compressor == nullptr) {
        return false;
    }

    std::string compressed(libdeflate_gzip_compress_bound(compressor, size), '\0');
    const auto compressed_size =
        libdeflate_gzip_compress(compressor, data.data(), size, compressed.data(), compressed.size());
    libdeflate_free_compressor(compressor);
    if (compressed_size == 0) {
        return false;
    }

    std::ofstream output(target, std::ios::binary);
    if (!output.write(compressed.data(), static_cast<std::streamsize>(compressed_size))) {
        return false;
    }
    output.close();

    std::filesystem::remove(src, ec);
    return !ec;
}
}  // namespace

TerminalConsoleAppender::TerminalConsoleAppender(std::FILE *target_file, spdlog::color_mode mode)
    : target_file_(target_file)
{
#ifdef _WIN32
    _setmode(_fileno(target_file_), _O_BINARY);
#endif
    constexpr auto red = "\033[31m";
    constexpr auto yellow = "\033[33m";

    setColorMode(mode);
    colors_.at(spdlog::level::warn) = yellow;
    colors_.at(spdlog::level::err) = red;
    colors_.at(spdlog::level::critical) = red;
}

void TerminalConsoleAppender::setColorMode(spdlog::color_mode mode)
{
    switch (mode) {
    case spdlog::color_mode::always:
        should_do_colors_ = true;
        break;
    case spdlog::color_mode::automatic:
        should_do_colors_ = spdlog::details::os::in_terminal(target_file_) && spdlog::details::os::is_color_terminal();
        break;
    case spdlog::color_mode::never:
    default:
        should_do_colors_ = false;
    }
    formatter_.add_flag<MinecraftFormattingConverter>('v', !should_do_colors_).add_flag<LevelFormatter>('L');
    formatter_.set_pattern("%^[%H:%M:%S %L]: %v%$");
}

void TerminalConsoleAppender::setReader(Console *console)
{
    Reader.store(console, std::memory_order_release);
}

void TerminalConsoleAppender::sink_it_(const spdlog::details::log_msg &msg)
{
    constexpr auto reset = "\033[m";

    msg.color_range_start = 0;
    msg.color_range_end = 0;

    spdlog::memory_buf_t formatted;
    formatter_.format(msg, formatted);

    std::string out;
    const auto &color = colors_.at(static_cast<std::size_t>(msg.level));
    if (should_do_colors_ && !color.empty() && msg.color_range_end > msg.color_range_start) {
        out.reserve(formatted.size() + color.size() + 8);
        out.append(formatted.data(), formatted.data() + msg.color_range_start);
        out.append(color);
        out.append(formatted.data() + msg.color_range_start, formatted.data() + msg.color_range_end);
        out.append(reset);
        out.append(formatted.data() + msg.color_range_end, formatted.data() + formatted.size());
    }
    else {
        out.assign(formatted.data(), formatted.size());
    }

    if (auto *console = Reader.load(std::memory_order_acquire)) {
        console->printAbove(out);
        return;
    }
    std::fwrite(out.data(), sizeof(char), out.size(), target_file_);
    std::fflush(target_file_);
}

void TerminalConsoleAppender::flush_()
{
    std::fflush(target_file_);
}

RollingRandomAccessFileAppender::RollingRandomAccessFileAppender(std::filesystem::path file_name,
                                                                 std::string file_pattern, std::uint16_t max_files)
    : file_name_(std::move(file_name)), file_pattern_(std::move(file_pattern)), max_files_(max_files)
{
    const bool should_rotate = std::filesystem::exists(file_name_);
    file_helper_.open(file_name_.string());
    if (should_rotate) {
        rotate();
    }
    rotation_tp_ = nextRotation();

    formatter_.add_flag<MinecraftFormattingConverter>('v', true);
    formatter_.add_flag<LevelFormatter>('L');
    formatter_.add_flag<ThreadFormatter>('t');
    formatter_.set_pattern("[%H:%M:%S] [%t/%L] [%n]: %v");
}

std::filesystem::path RollingRandomAccessFileAppender::calcFilename(std::size_t index) const
{
    if (index == 0) {
        return file_name_;
    }

    const std::tm tm = localtime(spdlog::log_clock::now());
    const std::chrono::year_month_day date{std::chrono::year{tm.tm_year + 1900} /
                                           std::chrono::month{static_cast<unsigned>(tm.tm_mon + 1)} /
                                           std::chrono::day{static_cast<unsigned>(tm.tm_mday)}};
    return std::vformat(file_pattern_, std::make_format_args(date, index));
}

void RollingRandomAccessFileAppender::sink_it_(const spdlog::details::log_msg &msg)
{
    if (msg.time >= rotation_tp_) {
        rotate();
        rotation_tp_ = nextRotation();
    }

    spdlog::memory_buf_t formatted;
    formatter_.format(msg, formatted);
    file_helper_.write(formatted);
}

void RollingRandomAccessFileAppender::flush_()
{
    file_helper_.flush();
}

void RollingRandomAccessFileAppender::rotate()
{
    file_helper_.flush();
    if (file_helper_.size() == 0) {
        return;
    }

    file_helper_.close();
    for (auto i = max_files_; i > 0; --i) {
        const auto src = calcFilename(i - 1);
        if (!std::filesystem::exists(src)) {
            continue;
        }

        const auto target = calcFilename(i);
        const auto move = (i == 1) ? compress : rename;
        if (!move(src, target)) {
            spdlog::details::os::sleep_for_millis(100);
            if (!move(src, target)) {
                break;
            }
        }
    }
    file_helper_.reopen(false);
}

}  // namespace endgate
