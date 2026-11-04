#include "endgate/os.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winver.h>
#else
#include <sched.h>
#include <unistd.h>

#include <filesystem>

#include <sys/statfs.h>
#include <sys/utsname.h>
#endif

namespace endgate::os {

namespace {

#ifdef _WIN32

struct WindowsVersion {
    int major = 0;
    int minor = 0;
    int build = 0;
    bool workstation = true;
};

const WindowsVersion &windowsVersion()
{
    static const WindowsVersion version = [] {
        using RtlGetVersion = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);

        RTL_OSVERSIONINFOEXW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        const auto function =
            reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (function != nullptr) {
            function(reinterpret_cast<PRTL_OSVERSIONINFOW>(&info));
        }
        WindowsVersion result{
            .major = static_cast<int>(info.dwMajorVersion),
            .minor = static_cast<int>(info.dwMinorVersion),
            .build = static_cast<int>(info.dwBuildNumber),
            .workstation = info.wProductType == VER_NT_WORKSTATION,
        };

        std::wstring kernel32(MAX_PATH, L'\0');
        const auto length = GetSystemDirectoryW(kernel32.data(), MAX_PATH - 14);
        if (length == 0 || length > MAX_PATH - 14) {
            return result;
        }
        kernel32.resize(length);
        kernel32 += L"\\kernel32.dll";
        const auto size = GetFileVersionInfoSizeW(kernel32.c_str(), nullptr);
        if (size == 0) {
            return result;
        }
        std::vector<std::byte> data(size);
        if (!GetFileVersionInfoW(kernel32.c_str(), 0, size, data.data())) {
            return result;
        }
        VS_FIXEDFILEINFO *file_info = nullptr;
        UINT file_info_length = 0;
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID *>(&file_info), &file_info_length)) {
            return result;
        }
        result.major = HIWORD(file_info->dwProductVersionMS);
        result.minor = LOWORD(file_info->dwProductVersionMS);
        result.build = HIWORD(file_info->dwProductVersionLS);
        return result;
    }();
    return version;
}

int popCount(ULONG_PTR mask)
{
    return std::popcount(mask);
}

int processorsInJobObject(DWORD &active_processor_groups)
{
    active_processor_groups = 0;
    BOOL in_job = FALSE;
    if (IsProcessInJob(GetCurrentProcess(), nullptr, &in_job) == 0 || in_job == FALSE) {
        return 0;
    }

    DWORD length = 0;
    if (QueryInformationJobObject(nullptr, JobObjectGroupInformationEx, nullptr, 0, &length) != 0 ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return 0;
    }
    std::vector<std::byte> data(length);
    if (QueryInformationJobObject(nullptr, JobObjectGroupInformationEx, data.data(), length, &length) == 0) {
        return 0;
    }
    int processors = 0;
    const auto *affinity = reinterpret_cast<const GROUP_AFFINITY *>(data.data());
    for (DWORD i = 0; i < length / sizeof(GROUP_AFFINITY); ++i) {
        const auto in_group = popCount(affinity[i].Mask);
        processors += in_group;
        if (in_group > 0) {
            ++active_processor_groups;
        }
    }
    return processors;
}

#else

struct CgroupController {
    bool v2 = false;
    std::string mount_point;
    std::string root;
    std::string cgroup_path;
    std::string path;

    void setSubsystemPath(const std::string &new_cgroup_path)
    {
        cgroup_path = new_cgroup_path;
        path = mount_point;
        if (v2) {
            if (cgroup_path != "/") {
                path += cgroup_path;
            }
            return;
        }
        if (root == "/") {
            if (cgroup_path != "/") {
                path += cgroup_path;
            }
        }
        else if (root != cgroup_path && !cgroup_path.empty() && cgroup_path != "/") {
            std::string_view suffix = cgroup_path;
            while (!suffix.empty()) {
                std::error_code error;
                if (std::filesystem::exists(mount_point + std::string(suffix), error)) {
                    path += suffix;
                    break;
                }
                const auto slash = suffix.find('/', 1);
                suffix = slash == std::string_view::npos ? std::string_view{} : suffix.substr(slash);
            }
        }
    }

    [[nodiscard]] bool needsHierarchyAdjustment() const { return v2 ? cgroup_path != "/" : root != cgroup_path; }
};

std::vector<std::string> splitOn(std::string_view text, std::string_view separators)
{
    std::vector<std::string> tokens;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find_first_of(separators, start);
        tokens.emplace_back(text.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return tokens;
}

std::optional<std::string> firstLine(const std::string &path)
{
    std::ifstream input(path);
    std::string line;
    if (!input || !std::getline(input, line)) {
        return std::nullopt;
    }
    return line;
}

std::optional<long long> parseLimit(const std::string &token)
{
    if (token == "max") {
        return -1;
    }
    try {
        std::size_t used = 0;
        const auto value = std::stoll(token, &used);
        return used == 0 ? std::nullopt : std::optional<long long>(value);
    }
    catch (const std::exception &) {
        return std::nullopt;
    }
}

constexpr int CONTAINER_ERROR = -2;

int cpuQuota(const CgroupController &controller)
{
    if (controller.v2) {
        const auto line = firstLine(controller.path + "/cpu.max");
        const auto quota = line ? parseLimit(splitOn(*line, " \t")[0]) : std::nullopt;
        return quota ? static_cast<int>(*quota) : CONTAINER_ERROR;
    }
    const auto line = firstLine(controller.path + "/cpu.cfs_quota_us");
    const auto quota = line ? parseLimit(*line) : std::nullopt;
    return quota ? static_cast<int>(*quota) : CONTAINER_ERROR;
}

int cpuPeriod(const CgroupController &controller)
{
    if (controller.v2) {
        const auto line = firstLine(controller.path + "/cpu.max");
        const auto tokens = line ? splitOn(*line, " \t") : std::vector<std::string>{};
        const auto period = tokens.size() > 1 ? parseLimit(tokens[1]) : std::nullopt;
        return period ? static_cast<int>(*period) : CONTAINER_ERROR;
    }
    const auto line = firstLine(controller.path + "/cpu.cfs_period_us");
    const auto period = line ? parseLimit(*line) : std::nullopt;
    return period ? static_cast<int>(*period) : CONTAINER_ERROR;
}

int processorCount(const CgroupController &controller, int host_cpus)
{
    return cgroup_processor_count(host_cpus, cpuQuota(controller), cpuPeriod(controller));
}

void adjustController(CgroupController &cpu, int host_cpus)
{
    if (cpu.cgroup_path.contains("../")) {
        cpu.setSubsystemPath("/");
        return;
    }
    if (!cpu.needsHierarchyAdjustment()) {
        return;
    }
    const std::string original = cpu.cgroup_path;
    std::string cg_path = original;
    int cpus = processorCount(cpu, host_cpus);
    int lowest_limit = std::min(cpus, host_cpus);
    const int original_limit = lowest_limit != host_cpus ? lowest_limit : host_cpus;
    std::optional<std::string> limit_cg_path;
    for (auto slash = cg_path.rfind('/'); slash != std::string::npos && slash != 0; slash = cg_path.rfind('/')) {
        cg_path.erase(slash);
        cpu.setSubsystemPath(cg_path);
        cpus = processorCount(cpu, host_cpus);
        if (cpus != host_cpus && cpus < lowest_limit) {
            lowest_limit = cpus;
            limit_cg_path = cg_path;
        }
    }
    cpu.setSubsystemPath("/");
    cpus = processorCount(cpu, host_cpus);
    if (cpus != host_cpus && cpus < lowest_limit) {
        lowest_limit = cpus;
        limit_cg_path = cg_path;
    }
    if (lowest_limit != original_limit && limit_cg_path) {
        cpu.setSubsystemPath(*limit_cg_path);
    }
    else {
        cpu.setSubsystemPath(original);
    }
}

struct MountInfo {
    std::string root;
    std::string mount_point;
    std::string fs_type;
    std::string super_options;
};

std::vector<MountInfo> readMountInfo()
{
    std::vector<MountInfo> mounts;
    std::ifstream input("/proc/self/mountinfo");
    std::string line;
    while (std::getline(input, line)) {
        const auto fields = splitOn(line, " ");
        const auto separator = std::ranges::find(fields, "-");
        if (fields.size() < 6 || separator == fields.end() || std::distance(separator, fields.end()) < 4) {
            continue;
        }
        mounts.push_back({.root = fields[3],
                          .mount_point = fields[4],
                          .fs_type = *(separator + 1),
                          .super_options = *(separator + 3)});
    }
    return mounts;
}

void setControllerPaths(std::optional<MountInfo> &slot, const MountInfo &mount)
{
    if (slot && slot->mount_point.starts_with("/sys/fs/cgroup")) {
        return;
    }
    slot = mount;
}

std::optional<CgroupController> findCpuController()
{
    constexpr long cgroup2_super_magic = 0x63677270;

    struct statfs fs{};
    const bool v2 = statfs("/sys/fs/cgroup", &fs) != -1 && fs.f_type == cgroup2_super_magic;

    std::ifstream controllers(v2 ? "/sys/fs/cgroup/cgroup.controllers" : "/proc/cgroups");
    if (!controllers) {
        return std::nullopt;
    }
    std::string line;
    std::vector<std::string> enabled;
    if (v2) {
        std::getline(controllers, line);
        enabled = splitOn(line, " \n\t\r\f\v");
    }
    else {
        while (std::getline(controllers, line)) {
            std::istringstream fields(line);
            std::string controller;
            int hierarchy_id = 0;
            int num_cgroups = 0;
            int is_enabled = 0;
            if ((fields >> controller >> hierarchy_id >> num_cgroups >> is_enabled) && is_enabled == 1) {
                enabled.push_back(controller);
            }
        }
    }
    const auto has = [&enabled](std::string_view name) {
        return std::ranges::contains(enabled, name);
    };
    const bool required =
        v2 ? has("cpu") && has("memory") : has("memory") && has("cpuset") && has("cpu") && has("cpuacct");
    if (!required) {
        return std::nullopt;
    }

    std::ifstream self_cgroup("/proc/self/cgroup");
    std::optional<std::string> cpu_cgroup_path;
    while (std::getline(self_cgroup, line)) {
        const auto first = line.find(':');
        const auto second = first == std::string::npos ? first : line.find(':', first + 1);
        if (second == std::string::npos) {
            continue;
        }
        const auto hierarchy_id = std::atoi(line.substr(0, first).c_str());
        const auto names = line.substr(first + 1, second - first - 1);
        const auto path = line.substr(second + 1);
        if (v2) {
            if (hierarchy_id == 0) {
                cpu_cgroup_path = path;
            }
        }
        else if (std::ranges::contains(splitOn(names, ","), "cpu")) {
            cpu_cgroup_path = path;
        }
    }
    if (!cpu_cgroup_path) {
        return std::nullopt;
    }

    std::optional<MountInfo> cpu_mount;
    bool memory_mounted = false;
    bool cpuset_mounted = false;
    bool cpuacct_mounted = false;
    for (const auto &mount : readMountInfo()) {
        if (v2) {
            if (mount.fs_type == "cgroup2") {
                setControllerPaths(cpu_mount, mount);
            }
            continue;
        }
        if (mount.fs_type != "cgroup") {
            continue;
        }
        for (const auto &token : splitOn(mount.super_options, ",")) {
            if (token == "cpu") {
                setControllerPaths(cpu_mount, mount);
            }
            memory_mounted = memory_mounted || token == "memory";
            cpuset_mounted = cpuset_mounted || token == "cpuset";
            cpuacct_mounted = cpuacct_mounted || token == "cpuacct";
        }
    }
    if (!cpu_mount || (!v2 && !(memory_mounted && cpuset_mounted && cpuacct_mounted))) {
        return std::nullopt;
    }

    CgroupController controller{.v2 = v2, .mount_point = cpu_mount->mount_point, .root = cpu_mount->root};
    controller.setSubsystemPath(*cpu_cgroup_path);
    return controller;
}

int schedulerProcessorCount()
{
    const auto configured = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    const auto online = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    cpu_set_t *const set = CPU_ALLOC(configured);
    if (set == nullptr) {
        return online;
    }
    const auto size = CPU_ALLOC_SIZE(configured);
    CPU_ZERO_S(size, set);
    const int count = sched_getaffinity(0, size, set) == 0 ? CPU_COUNT_S(size, set) : online;
    CPU_FREE(set);
    return count;
}

#endif

}  // namespace

std::string name()
{
#ifdef _WIN32
    const auto &version = windowsVersion();
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    return windows_name(version.major, version.minor, version.build, version.workstation,
                        info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64);
#else
    utsname info{};
    uname(&info);
    return info.sysname;
#endif
}

std::string version()
{
#ifdef _WIN32
    const auto &version = windowsVersion();
    return std::format("{}.{}", version.major, version.minor);
#else
    utsname info{};
    uname(&info);
    return info.release;
#endif
}

std::string arch()
{
#if defined(_M_AMD64) || defined(__x86_64__)
    return "amd64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64";
#else
    return "unknown";
#endif
}

std::string windows_name(int major, int minor, int build, bool workstation, bool is_64bit)
{
    if (major <= 4) {
        return "Windows NT";
    }
    if (major == 5) {
        switch (minor) {
        case 0:
            return "Windows 2000";
        case 1:
            return "Windows XP";
        case 2:
            return workstation && is_64bit ? "Windows XP" : "Windows 2003";
        default:
            return "Windows NT (unknown)";
        }
    }
    if (major == 6) {
        if (workstation) {
            switch (minor) {
            case 0:
                return "Windows Vista";
            case 1:
                return "Windows 7";
            case 2:
                return "Windows 8";
            case 3:
                return "Windows 8.1";
            default:
                return "Windows NT (unknown)";
            }
        }
        switch (minor) {
        case 0:
            return "Windows Server 2008";
        case 1:
            return "Windows Server 2008 R2";
        case 2:
            return "Windows Server 2012";
        case 3:
            return "Windows Server 2012 R2";
        default:
            return "Windows NT (unknown)";
        }
    }
    if (major == 10 && minor == 0) {
        if (workstation) {
            return build >= 22000 ? "Windows 11" : "Windows 10";
        }
        if (build > 26039) {
            return "Windows Server 2025";
        }
        if (build > 20347) {
            return "Windows Server 2022";
        }
        if (build > 17762) {
            return "Windows Server 2019";
        }
        return "Windows Server 2016";
    }
    return "Windows NT (unknown)";
}

int cgroup_processor_count(int host_cpus, int quota, int period)
{
    int limit_count = host_cpus;
    int quota_count = 0;
    if (quota > -1 && period > 0) {
        quota_count = static_cast<int>(std::ceil(static_cast<float>(quota) / static_cast<float>(period)));
    }
    if (quota_count != 0) {
        limit_count = quota_count;
    }
    return std::min(host_cpus, limit_count);
}

int active_processor_count()
{
#ifdef _WIN32
    const auto &version = windowsVersion();
    const bool schedules_all_processor_groups =
        version.workstation ? (version.major == 10 && version.build >= 22000) || version.major > 10
                            : (version.major == 10 && version.build >= 20348) || version.major > 10;

    DWORD active_processor_groups = 0;
    const int processors_in_job_object = processorsInJobObject(active_processor_groups);
    if (processors_in_job_object > 0) {
        if (!schedules_all_processor_groups || active_processor_groups <= 1) {
            return processors_in_job_object;
        }
    }

    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto system_processors = static_cast<int>(info.dwNumberOfProcessors);

    USHORT group_count = 0;
    if (GetProcessGroupAffinity(GetCurrentProcess(), &group_count, nullptr) != 0 ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || group_count == 0) {
        return system_processors;
    }
    if (group_count == 1) {
        DWORD_PTR process_mask = 0;
        DWORD_PTR system_mask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) != 0) {
            const int logical_processors = popCount(process_mask);
            if (logical_processors > 0) {
                return logical_processors;
            }
        }
    }
    return system_processors;
#else
    const int host_cpus = schedulerProcessorCount();
    auto cpu = findCpuController();
    if (!cpu) {
        return host_cpus;
    }
    adjustController(*cpu, host_cpus);
    return processorCount(*cpu, host_cpus);
#endif
}

}  // namespace endgate::os
