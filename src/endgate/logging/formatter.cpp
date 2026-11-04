#include "endgate/logging/formatter.h"

#include <cctype>
#include <string>
#include <string_view>

#include <spdlog/details/fmt_helper.h>

#include "endgate/thread.h"

namespace endgate {

namespace {

constexpr std::string_view SECTION_SIGN = "\xC2\xA7";
constexpr std::string_view RESET = "\x1b[0m";

std::string_view ansiFor(char code)
{
    switch (std::tolower(static_cast<unsigned char>(code))) {
    case '0':
        return "\x1b[30m";
    case '1':
        return "\x1b[34m";
    case '2':
        return "\x1b[32m";
    case '3':
        return "\x1b[36m";
    case '4':
        return "\x1b[31m";
    case '5':
        return "\x1b[35m";
    case '6':
        return "\x1b[33m";
    case '7':
        return "\x1b[37m";
    case '8':
        return "\x1b[90m";
    case '9':
        return "\x1b[94m";
    case 'a':
        return "\x1b[92m";
    case 'b':
        return "\x1b[96m";
    case 'c':
        return "\x1b[91m";
    case 'd':
        return "\x1b[95m";
    case 'e':
        return "\x1b[93m";
    case 'f':
        return "\x1b[97m";
    case 'g':
        return "\x1b[38;2;221;214;5m";
    case 'h':
        return "\x1b[38;2;227;212;209m";
    case 'i':
        return "\x1b[38;2;206;202;202m";
    case 'j':
        return "\x1b[38;2;68;58;59m";
    case 'm':
        return "\x1b[38;2;151;22;7m";
    case 'n':
        return "\x1b[38;2;180;104;77m";
    case 'p':
        return "\x1b[38;2;222;177;45m";
    case 'q':
        return "\x1b[38;2;17;160;54m";
    case 's':
        return "\x1b[38;2;44;186;168m";
    case 't':
        return "\x1b[38;2;33;73;123m";
    case 'u':
        return "\x1b[38;2;154;92;198m";
    case 'v':
        return "\x1b[38;2;234;113;19m";
    case 'k':
        return "\x1b[8m";
    case 'l':
        return "\x1b[1m";
    case 'o':
        return "\x1b[3m";
    case 'r':
        return RESET;
    default:
        return {};
    }
}

std::size_t graphicRenditionEnd(std::string_view input, std::size_t begin)
{
    if (begin + 1 >= input.size() || input[begin + 1] != '[') {
        return begin;
    }
    auto i = begin + 2;
    while (i < input.size() && (input[i] == ';' || std::isdigit(static_cast<unsigned char>(input[i])) != 0)) {
        i++;
    }
    return i < input.size() && input[i] == 'm' ? i + 1 : begin;
}

}  // namespace

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

std::string MinecraftFormattingConverter::convert(std::string_view message, bool strip)
{
    std::string result;
    result.reserve(message.size());
    bool formatted = false;
    for (std::size_t i = 0; i < message.size(); i++) {
        if (message.compare(i, SECTION_SIGN.size(), SECTION_SIGN) == 0 && i + SECTION_SIGN.size() < message.size()) {
            const std::string_view ansi = ansiFor(message[i + SECTION_SIGN.size()]);
            if (!ansi.empty()) {
                if (!strip) {
                    result.append(ansi);
                    formatted = true;
                }
                i += SECTION_SIGN.size();
                continue;
            }
        }
        else if (strip && message[i] == '\x1b') {
            if (const auto end = graphicRenditionEnd(message, i); end != i) {
                i = end - 1;
                continue;
            }
        }
        result.push_back(message[i]);
    }
    if (formatted) {
        result.append(RESET);
    }
    return result;
}

void MinecraftFormattingConverter::format(const spdlog::details::log_msg &msg, const std::tm &,
                                          spdlog::memory_buf_t &dest)
{
    const std::string converted = convert({msg.payload.data(), msg.payload.size()}, strip_);
    spdlog::details::fmt_helper::append_string_view(converted, dest);
}

std::unique_ptr<spdlog::custom_flag_formatter> MinecraftFormattingConverter::clone() const
{
    return std::make_unique<MinecraftFormattingConverter>(strip_);
}

}  // namespace endgate
