#include "endgate/metrics.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "endgate/bstats/config.h"
#include "endgate/logging/manager.h"
#include "endgate/os.h"
#include "endgate/server.h"

namespace endgate {

namespace {

void appendPlatformData(JsonObject &builder)
{
    builder["osName"] = os::name();
    builder["osArch"] = os::arch();
    builder["osVersion"] = os::version();
    builder["coreCount"] = os::active_processor_count();
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
        logger.info("");
        logger.info("Endgate collects metrics and sends them to bStats (https://bStats.org).");
        logger.info("bStats collects some basic information for server authors, like how many people use");
        logger.info("Endgate and their total player count. It's recommended to keep bStats enabled, but");
        logger.info("if you're not comfortable with this, you can opt-out by editing the config.toml file in");
        logger.info("the 'plugins/bstats' folder and setting enabled to false.");
        logger.info("");
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
        return std::map<std::string, std::map<std::string, int>>{{os::name(), {{os::version(), 1}}}};
    }));
    return metrics;
}

}  // namespace endgate
