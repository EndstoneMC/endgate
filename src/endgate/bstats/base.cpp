#include "endgate/bstats/base.h"

#include <libdeflate.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <future>
#include <random>
#include <span>
#include <stdexcept>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <glaze/json.hpp>

namespace endgate {

namespace {

constexpr std::chrono::milliseconds PERIOD{std::chrono::minutes{30}};

double random()
{
    thread_local std::mt19937_64 engine{std::random_device{}()};
    return std::uniform_real_distribution<double>(0.0, 1.0)(engine);
}

std::string compress(std::string_view text)
{
    constexpr int java_default_level = 6;

    auto *const compressor = libdeflate_alloc_compressor(java_default_level);
    if (compressor == nullptr) {
        throw std::runtime_error("unable to allocate a compressor");
    }
    std::string compressed(libdeflate_gzip_compress_bound(compressor, text.size()), '\0');
    const auto size =
        libdeflate_gzip_compress(compressor, text.data(), text.size(), compressed.data(), compressed.size());
    libdeflate_free_compressor(compressor);
    if (size == 0) {
        throw std::runtime_error("unable to compress the data");
    }
    compressed.resize(size);
    return compressed;
}

struct Submission {
    HttpClient client;
    std::string url;
    std::string data;
    ErrorLogger error_logger;
    std::function<void(const std::string &)> info_logger;
    bool log_errors;
    bool log_sent_data;
    bool log_response_status_text;
};

Awaitable<void> sendData(Submission submission)
{
    try {
        if (submission.log_sent_data) {
            submission.info_logger("Sent bStats metrics data: " + submission.data);
        }

        HttpRequest request{
            .uri = submission.url,
            .headers =
                {
                    {"Accept", "application/json"},
                    {"Content-Encoding", "gzip"},
                    {"Content-Type", "application/json"},
                    {"User-Agent", "Metrics-Service/1"},
                },
            .method = "POST",
            .body = compress(submission.data),
        };
        const auto response = co_await submission.client.sendAsync(std::move(request));
        if (!response) {
            throw std::runtime_error(response.error());
        }
        if (response->statusCode() / 100 != 2) {
            throw std::runtime_error(std::format("Server returned HTTP response code: {} for URL: {}",
                                                 response->statusCode(), submission.url));
        }
        if (submission.log_response_status_text) {
            std::string body = response->body();
            std::erase_if(body, [](char c) { return c == '\r' || c == '\n'; });
            submission.info_logger("Sent data to bStats and received response: " + body);
        }
    }
    catch (const std::exception &error) {
        if (submission.log_errors) {
            submission.error_logger("Could not submit bStats metrics data", error);
        }
    }
}

}  // namespace

MetricsBase::MetricsBase(std::string platform, std::string server_uuid, int service_id, bool enabled,
                         std::function<void(JsonObject &)> append_platform_data_consumer,
                         std::function<void(JsonObject &)> append_service_data_consumer,
                         std::function<void(std::function<void()>)> submit_task_consumer,
                         std::function<bool()> check_service_enabled_supplier, ErrorLogger error_logger,
                         std::function<void(const std::string &)> info_logger, bool log_errors, bool log_sent_data,
                         bool log_response_status_text, HttpClient client, Overrides overrides)
    : platform_(std::move(platform)), server_uuid_(std::move(server_uuid)), service_id_(service_id), enabled_(enabled),
      append_platform_data_consumer_(std::move(append_platform_data_consumer)),
      append_service_data_consumer_(std::move(append_service_data_consumer)),
      submit_task_consumer_(std::move(submit_task_consumer)),
      check_service_enabled_supplier_(std::move(check_service_enabled_supplier)),
      error_logger_(std::move(error_logger)), info_logger_(std::move(info_logger)), log_errors_(log_errors),
      log_sent_data_(log_sent_data), log_response_status_text_(log_response_status_text), client_(std::move(client)),
      overrides_(std::move(overrides))
{
    if (enabled_) {
        startSubmitting();
    }
}

MetricsBase::~MetricsBase()
{
    shutdown();
}

void MetricsBase::addCustomChart(std::unique_ptr<CustomChart> chart)
{
    const std::scoped_lock lock(charts_mutex_);
    custom_charts_.push_back(std::move(chart));
}

void MetricsBase::shutdown()
{
    constexpr std::chrono::seconds barrier_timeout{5};

    if (shut_down_.exchange(true) || !enabled_) {
        return;
    }
    const auto drain = [this] {
        stop();
        alive_.reset();
    };
    auto &loop = client_.executor();
    if (loop.inEventLoop()) {
        drain();
        return;
    }
    const auto drained = std::make_shared<std::promise<void>>();
    auto done = drained->get_future();
    loop.execute([drain, drained] {
        drain();
        drained->set_value();
    });
    done.wait_for(barrier_timeout);
}

void MetricsBase::startSubmitting()
{
    const auto initial_delay = overrides_.initial_delay.value_or(
        std::chrono::milliseconds{static_cast<std::int64_t>(1000 * 60 * (3 + random() * 3))});
    const auto second_delay = std::chrono::milliseconds{static_cast<std::int64_t>(1000 * 60 * (random() * 30))};
    schedule(0, initial_delay, false);
    schedule(1, initial_delay + second_delay, true);
}

void MetricsBase::schedule(std::size_t slot, std::chrono::milliseconds delay, bool periodic)
{
    auto future = client_.executor().schedule(
        [this, alive = std::weak_ptr<void>(alive_), slot, periodic] {
            const auto guard = alive.lock();
            if (!guard || stopped_) {
                return;
            }
            if (periodic) {
                schedule(slot, PERIOD, true);
            }
            submitTask();
        },
        delay);
    const std::scoped_lock lock(futures_mutex_);
    futures_[slot] = std::move(future);
}

void MetricsBase::submitTask()
{
    if (!enabled_ || !check_service_enabled_supplier_()) {
        stop();
        return;
    }
    if (submit_task_consumer_) {
        submit_task_consumer_([this, alive = std::weak_ptr<void>(alive_)] {
            if (const auto guard = alive.lock()) {
                submitData();
            }
        });
    }
    else {
        submitData();
    }
}

void MetricsBase::stop()
{
    stopped_ = true;
    const std::scoped_lock lock(futures_mutex_);
    for (const auto &future : futures_) {
        if (future) {
            future->cancel();
        }
    }
}

void MetricsBase::submitData()
{
    JsonObject base_json;
    append_platform_data_consumer_(base_json);

    JsonObject service_json;
    append_service_data_consumer_(service_json);

    std::vector<JsonObject> chart_data;
    {
        const std::scoped_lock lock(charts_mutex_);
        for (const auto &chart : custom_charts_) {
            if (auto data = chart->getRequestJsonObject(error_logger_, log_errors_)) {
                chart_data.push_back(std::move(*data));
            }
        }
    }

    service_json["id"] = service_id_;
    service_json["customCharts"] = chart_data;
    base_json["service"] = std::move(service_json);
    base_json["serverUUID"] = server_uuid_;
    base_json["metricsVersion"] = std::string(METRICS_VERSION);

    auto data = glz::write_json(base_json);
    if (!data) {
        if (log_errors_) {
            error_logger_("Could not submit bStats metrics data", std::runtime_error(glz::format_error(data.error())));
        }
        return;
    }
    auto url = overrides_.report_url.empty() ? std::string(REPORT_URL) + platform_ : overrides_.report_url;
    asio::co_spawn(client_.executor().getExecutor(),
                   sendData({.client = client_,
                             .url = std::move(url),
                             .data = std::move(*data),
                             .error_logger = error_logger_,
                             .info_logger = info_logger_,
                             .log_errors = log_errors_,
                             .log_sent_data = log_sent_data_,
                             .log_response_status_text = log_response_status_text_}),
                   asio::detached);
}

}  // namespace endgate
