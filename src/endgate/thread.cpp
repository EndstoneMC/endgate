#include "endgate/thread.h"

#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace endgate {

namespace {
thread_local std::string CurrentName;
}  // namespace

std::string Thread::getName()
{
#ifdef _WIN32
    using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    static const auto get_thread_description = [] {
        auto *const kernel32 = GetModuleHandleW(L"kernel32.dll");
        return kernel32 == nullptr
                 ? nullptr
                 : reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(kernel32, "GetThreadDescription"));
    }();
    if (get_thread_description == nullptr) {
        return CurrentName;
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
    if (!CurrentName.empty()) {
        return CurrentName;
    }
    char buffer[64];
    if (pthread_getname_np(pthread_self(), buffer, sizeof(buffer)) != 0) {
        return {};
    }
    return buffer;
#endif
}

void Thread::setName(std::string name)
{
#ifdef _WIN32
    using SetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    static const auto set_thread_description = [] {
        auto *const kernel32 = GetModuleHandleW(L"kernel32.dll");
        return kernel32 == nullptr
                 ? nullptr
                 : reinterpret_cast<SetThreadDescriptionFn>(GetProcAddress(kernel32, "SetThreadDescription"));
    }();
    if (set_thread_description != nullptr) {
        const auto length = static_cast<int>(name.size());
        const auto size = MultiByteToWideChar(CP_UTF8, 0, name.data(), length, nullptr, 0);
        std::wstring description(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name.data(), length, description.data(), size);
        set_thread_description(GetCurrentThread(), description.c_str());
        return;
    }
#else
    constexpr std::size_t os_name_limit = 15;
    pthread_setname_np(pthread_self(), name.substr(0, os_name_limit).c_str());
#endif
    CurrentName = std::move(name);
}

}  // namespace endgate
