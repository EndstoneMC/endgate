#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "endgate/bstats/charts.h"
#include "endgate/network/http.h"

namespace endgate {

/**
 * Sends the metrics of a service to bStats on a schedule of its own.
 *
 * @see org.bstats.MetricsBase
 */
class MetricsBase {
public:
    /**
     * The version of the Metrics class.
     *
     * @see org.bstats.MetricsBase#METRICS_VERSION
     */
    static constexpr std::string_view METRICS_VERSION = "3.0.3";

    static constexpr std::string_view REPORT_URL = "https://bStats.org/api/v2/data/";

    /**
     * Where the data goes and when it first goes there, for a test that points it at a fake of bStats.
     */
    struct Overrides {
        std::string report_url;
        std::optional<std::chrono::milliseconds> initial_delay;
    };

    /**
     * Creates a new MetricsBase class instance.
     *
     * @param platform The platform of the service.
     * @param server_uuid The server uuid.
     * @param service_id The id of the service.
     * @param enabled Whether or not data sending is enabled.
     * @param append_platform_data_consumer A consumer that receives a JsonObject and appends all
     * platform-specific data.
     * @param append_service_data_consumer A consumer that receives a JsonObject and appends all service-specific
     * data.
     * @param submit_task_consumer A consumer that takes a task with the submit task. This can be used to delegate the
     * data collection to another thread to prevent errors caused by concurrency. Can be empty.
     * @param check_service_enabled_supplier A supplier to check if the service is still enabled.
     * @param error_logger A consumer that accepts log message and an error.
     * @param info_logger A consumer that accepts info log messages.
     * @param log_errors Whether or not errors should be logged.
     * @param log_sent_data Whether or not the sent data should be logged.
     * @param log_response_status_text Whether or not the response status text should be logged.
     * @param client The client that sends the data, whose event loop runs the schedule.
     * @param overrides Where the data goes and when it first goes there.
     *
     * @see org.bstats.MetricsBase#MetricsBase
     */
    MetricsBase(std::string platform, std::string server_uuid, int service_id, bool enabled,
                std::function<void(JsonObject &)> append_platform_data_consumer,
                std::function<void(JsonObject &)> append_service_data_consumer,
                std::function<void(std::function<void()>)> submit_task_consumer,
                std::function<bool()> check_service_enabled_supplier, ErrorLogger error_logger,
                std::function<void(const std::string &)> info_logger, bool log_errors, bool log_sent_data,
                bool log_response_status_text, HttpClient client, Overrides overrides = {});
    MetricsBase(const MetricsBase &) = delete;
    MetricsBase &operator=(const MetricsBase &) = delete;
    MetricsBase(MetricsBase &&) = delete;
    MetricsBase &operator=(MetricsBase &&) = delete;
    ~MetricsBase();

    /**
     * @see org.bstats.MetricsBase#addCustomChart
     */
    void addCustomChart(std::unique_ptr<CustomChart> chart);

    /**
     * Stops the schedule, waiting for a run that is already queued. Must not be called from an event loop.
     *
     * @see org.bstats.MetricsBase#shutdown
     */
    void shutdown();

private:
    void startSubmitting();
    void schedule(std::size_t slot, std::chrono::milliseconds delay, bool periodic);
    void submitTask();
    void submitData();
    void stop();

    std::string platform_;
    std::string server_uuid_;
    int service_id_;
    bool enabled_;
    std::function<void(JsonObject &)> append_platform_data_consumer_;
    std::function<void(JsonObject &)> append_service_data_consumer_;
    std::function<void(std::function<void()>)> submit_task_consumer_;
    std::function<bool()> check_service_enabled_supplier_;
    ErrorLogger error_logger_;
    std::function<void(const std::string &)> info_logger_;
    bool log_errors_;
    bool log_sent_data_;
    bool log_response_status_text_;
    HttpClient client_;
    Overrides overrides_;
    std::mutex charts_mutex_;
    std::vector<std::unique_ptr<CustomChart>> custom_charts_;
    std::mutex futures_mutex_;
    std::array<std::shared_ptr<ScheduledFuture>, 2> futures_;
    std::shared_ptr<void> alive_ = std::make_shared<char>();
    std::atomic<bool> shut_down_ = false;
    bool stopped_ = false;
};

}  // namespace endgate
