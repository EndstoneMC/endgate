#pragma once

#include <memory>

#include <spdlog/logger.h>

#include "endgate/bstats/base.h"
#include "endgate/bstats/charts.h"
#include "endgate/network/http.h"

namespace endgate {

class ProxyServer;

/**
 * Initializes bStats.
 *
 * @see com.velocitypowered.proxy.Metrics
 */
class Metrics {
public:
    Metrics(spdlog::logger &logger, HttpClient client, int service_id, bool default_enabled,
            MetricsBase::Overrides overrides = {});

    /**
     * Adds a custom chart.
     *
     * @see com.velocitypowered.proxy.Metrics#addCustomChart
     */
    void addCustomChart(std::unique_ptr<CustomChart> chart);

    /**
     * Stops sending metrics. Must not be called from an event loop.
     *
     * @see org.bstats.MetricsBase#shutdown
     */
    void shutdown();

    /**
     * @see com.velocitypowered.proxy.Metrics.VelocityMetrics
     */
    class EndgateMetrics {
    public:
        static constexpr int SERVICE_ID = 30353;

        /**
         * Starts sending the metrics of the proxy.
         *
         * @see com.velocitypowered.proxy.Metrics.VelocityMetrics#startMetrics
         */
        static std::unique_ptr<Metrics> startMetrics(ProxyServer &server, bool default_enabled);
    };

private:
    std::unique_ptr<MetricsBase> metrics_base_;
};

}  // namespace endgate
