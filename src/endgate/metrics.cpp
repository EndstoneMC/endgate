#include "endgate/metrics.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#include "endgate/bstats/config.h"
#include "endgate/logging/manager.h"
#include "endgate/server.h"

namespace endgate {

namespace {

struct OperatingSystem {
    std::string family;
    std::string name;
    std::string version;
    std::string release;
};

OperatingSystem detectOperatingSystem()
{
#ifdef _WIN32
    using RtlGetVersion = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);

    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    const auto function =
        reinterpret_cast<RtlGetVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (function != nullptr) {
        function(&info);
    }
    constexpr DWORD first_windows_11_build = 22000;
    const auto *const name = info.dwBuildNumber >= first_windows_11_build ? "Windows 11" : "Windows 10";
    return {
        .family = "Windows",
        .name = name,
        .version = std::format("{}.{}", info.dwMajorVersion, info.dwMinorVersion),
        .release = std::format("{} ({}.{}.{})", name, info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber),
    };
#else
    utsname info{};
    uname(&info);
    return {
        .family = info.sysname,
        .name = info.sysname,
        .version = info.release,
        .release = std::format("{} {}", info.sysname, info.release),
    };
#endif
}

const OperatingSystem &operatingSystem()
{
    static const OperatingSystem system = detectOperatingSystem();
    return system;
}

std::string architecture()
{
#if defined(__x86_64__) || defined(_M_X64)
    return "amd64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#else
    return "unknown";
#endif
}

void appendPlatformData(JsonObject &builder)
{
    const auto &system = operatingSystem();
    builder["osName"] = system.name;
    builder["osArch"] = architecture();
    builder["osVersion"] = system.version;
    builder["coreCount"] = static_cast<int>(std::thread::hardware_concurrency());
}

}  // namespace

Metrics::Metrics(spdlog::logger &logger, HttpClient client, int service_id, bool default_enabled,
                 MetricsBase::Overrides overrides)
{
    std::optional<MetricsConfig> config;
    try {
        config.emplace(std::filesystem::path("plugins") / "bstats" / "config.toml", default_enabled);
    }
    catch (const std::exception &error) {
        logger.error("Failed to create bStats config: {}", error.what());
        return;
    }

    metrics_base_ = std::make_unique<MetricsBase>(
        "server-implementation", config->getServerUUID(), service_id, config->isEnabled(), appendPlatformData,
        [](JsonObject &) {}, nullptr, [] { return true; },
        [&logger](const std::string &message, const std::exception &error) {
            logger.warn("{}: {}", message, error.what());
        },
        [&logger](const std::string &message) { logger.info("{}", message); }, config->isLogErrorsEnabled(),
        config->isLogSentDataEnabled(), config->isLogResponseStatusTextEnabled(), std::move(client),
        std::move(overrides));

    if (!config->didExistBefore()) {
        logger.info("Endgate collects metrics and sends them to bStats (https://bStats.org).");
        logger.info("bStats collects some basic information for server authors, like how many people use");
        logger.info("Endgate and their total player count. It's recommended to keep bStats enabled, but");
        logger.info("if you're not comfortable with this, you can opt-out by editing the config.toml file in");
        logger.info("the 'plugins/bstats' folder and setting enabled to false.");
    }
}

void Metrics::addCustomChart(std::unique_ptr<CustomChart> chart)
{
    if (metrics_base_) {
        metrics_base_->addCustomChart(std::move(chart));
    }
}

void Metrics::shutdown()
{
    if (metrics_base_) {
        metrics_base_->shutdown();
    }
}

std::unique_ptr<Metrics> Metrics::EndgateMetrics::startMetrics(ProxyServer &server, bool default_enabled)
{
    auto &logger = LogManager::getLogger("Metrics");

    MetricsBase::Overrides overrides;
    if (const auto *const override_uri = std::getenv("ENDGATE_BSTATS_URI")) {
        logger.warn("Metrics are sent to {} instead of bStats.", override_uri);
        overrides.report_url = override_uri;
        overrides.initial_delay = std::chrono::milliseconds{0};
    }

    auto metrics =
        std::make_unique<Metrics>(logger, server.createHttpClient(), SERVICE_ID, default_enabled, std::move(overrides));

    metrics->addCustomChart(
        std::make_unique<SingleLineChart>("players", [&server] { return static_cast<int>(server.getPlayerCount()); }));
    metrics->addCustomChart(std::make_unique<SingleLineChart>(
        "managed_servers", [&server] { return static_cast<int>(server.getAllServers().size()); }));
    metrics->addCustomChart(std::make_unique<SimplePie>(
        "online_mode", [&server] { return server.getConfiguration().isOnlineMode() ? "online" : "offline"; }));
    metrics->addCustomChart(
        std::make_unique<SimplePie>("endgate_version", [] { return ProxyServer::getVersion().getVersion(); }));
    metrics->addCustomChart(std::make_unique<DrilldownPie>("operating_system", [] {
        const auto &system = operatingSystem();
        return std::map<std::string, std::map<std::string, int>>{{system.family, {{system.release, 1}}}};
    }));
    return metrics;
}

}  // namespace endgate
