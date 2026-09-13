#include "endgate/logging/formatter.h"

#include <string>
#include <string_view>

#include <spdlog/details/fmt_helper.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace endgate {

namespace {
std::string getThreadName()
{
#ifdef _WIN32
    using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    static const auto get_thread_description = [] {
        auto *const kernel32 = GetModuleHandleA("Kernel32.dll");
        return kernel32 == nullptr
                 ? nullptr
                 : reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(kernel32, "GetThreadDescription"));
    }();
    if (get_thread_description == nullptr) {
        return {};
    }

    PWSTR description = nullptr;
    if (FAILED(get_thread_description(GetCurrentThread(), &description)) || description == nullptr) {
        return {};
    }
    const auto size = WideCharToMultiByte(CP_UTF8, 0, description, -1, nullptr, 0, nullptr, nullptr);
    std::string name(size > 0 ? size - 1 : 0, '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, description, -1, name.data(), size, nullptr, nullptr);
    }
    LocalFree(description);
    return name;
#else
    char buffer[64];
    if (pthread_getname_np(pthread_self(), buffer, sizeof(buffer)) != 0) {
        return {};
    }
    return buffer;
#endif
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
    if (const auto name = getThreadName(); !name.empty()) {
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
