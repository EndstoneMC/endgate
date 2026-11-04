#include <libdeflate.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <istream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>

#include "endgate/bstats/base.h"
#include "endgate/network/eventloop.h"

namespace endgate {
namespace {

using namespace std::chrono_literals;

struct Received {
    std::string request_line;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    [[nodiscard]] std::string header(const std::string &name) const
    {
        for (const auto &[key, value] : headers) {
            if (key == name) {
                return value;
            }
        }
        return {};
    }
};

class FakeBStats {
public:
    explicit FakeBStats(int status = 200, std::string answer = "{}")
        : acceptor_(io_context_, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)), status_(status),
          answer_(std::move(answer)), thread_([this] { serve(); })
    {
    }

    FakeBStats(const FakeBStats &) = delete;
    FakeBStats &operator=(const FakeBStats &) = delete;
    FakeBStats(FakeBStats &&) = delete;
    FakeBStats &operator=(FakeBStats &&) = delete;

    ~FakeBStats()
    {
        io_context_.stop();
        acceptor_.close();
    }

    [[nodiscard]] std::string url() const
    {
        return "http://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port()) + "/api/v2/data/test";
    }

    [[nodiscard]] bool waitForRequest(std::chrono::milliseconds timeout)
    {
        return arrived_.get_future().wait_for(timeout) == std::future_status::ready;
    }

    [[nodiscard]] std::size_t count()
    {
        const std::scoped_lock lock(mutex_);
        return received_.size();
    }

    [[nodiscard]] Received first()
    {
        const std::scoped_lock lock(mutex_);
        return received_.front();
    }

private:
    void serve()
    {
        while (!io_context_.stopped()) {
            asio::ip::tcp::socket socket(io_context_);
            asio::error_code error;
            error = acceptor_.accept(socket, error);
            if (error) {
                return;
            }
            asio::streambuf buffer;
            asio::read_until(socket, buffer, "\r\n\r\n", error);
            if (error) {
                continue;
            }
            std::istream stream(&buffer);
            Received received;
            std::getline(stream, received.request_line);
            std::string line;
            while (std::getline(stream, line) && line != "\r") {
                const auto colon = line.find(':');
                received.headers.emplace_back(line.substr(0, colon), line.substr(colon + 2, line.size() - colon - 3));
            }
            const auto length = static_cast<std::size_t>(std::stoul(received.header("Content-Length")));
            received.body.resize(length);
            const auto buffered = buffer.sgetn(received.body.data(), static_cast<std::streamsize>(length));
            asio::read(socket,
                       asio::buffer(received.body.data() + buffered, length - static_cast<std::size_t>(buffered)),
                       error);

            const auto response = "HTTP/1.1 " + std::to_string(status_) +
                                  " X\r\nContent-Length: " + std::to_string(answer_.size() + 2) +
                                  "\r\nConnection: close\r\n\r\n" + answer_ + "\r\n";
            asio::write(socket, asio::buffer(response), error);
            {
                const std::scoped_lock lock(mutex_);
                received_.push_back(std::move(received));
            }
            if (!notified_.exchange(true)) {
                arrived_.set_value();
            }
        }
    }

    asio::io_context io_context_;
    asio::ip::tcp::acceptor acceptor_;
    int status_;
    std::string answer_;
    std::mutex mutex_;
    std::vector<Received> received_;
    std::atomic<bool> notified_ = false;
    std::promise<void> arrived_;
    std::jthread thread_;
};

std::string gunzip(const std::string &compressed)
{
    auto *const decompressor = libdeflate_alloc_decompressor();
    std::string text(1 << 20, '\0');
    std::size_t size = 0;
    const auto result =
        libdeflate_gzip_decompress(decompressor, compressed.data(), compressed.size(), text.data(), text.size(), &size);
    libdeflate_free_decompressor(decompressor);
    EXPECT_EQ(result, LIBDEFLATE_SUCCESS);
    text.resize(size);
    return text;
}

struct Logs {
    std::mutex mutex;
    std::vector<std::string> errors;
    std::vector<std::string> infos;
};

class MetricsBaseTest : public ::testing::Test {
protected:
    [[nodiscard]] std::unique_ptr<MetricsBase> make(
        const FakeBStats &server, bool enabled, bool log_errors = false, bool log_sent_data = false,
        bool log_response = false, std::chrono::milliseconds delay = 0ms,
        std::function<bool()> service_enabled = [] { return true; })
    {
        auto metrics = std::make_unique<MetricsBase>(
            "server-implementation", "00000000-0000-4000-8000-000000000000", 42, enabled,
            [](JsonObject &builder) { builder["osName"] = "Test"; },
            [](JsonObject &builder) { builder["pluginVersion"] = "1"; }, nullptr, std::move(service_enabled),
            [this](const std::string &message, const std::exception &error) {
                const std::scoped_lock lock(logs_.mutex);
                logs_.errors.push_back(message + ": " + error.what());
            },
            [this](const std::string &message) {
                const std::scoped_lock lock(logs_.mutex);
                logs_.infos.push_back(message);
            },
            log_errors, log_sent_data, log_response, HttpClient(loop_, {}),
            MetricsBase::Overrides{.report_url = server.url(), .initial_delay = delay});
        metrics->addCustomChart(std::make_unique<SimplePie>("chart", [] { return "value"; }));
        return metrics;
    }

    EventLoop loop_{"Test Worker"};
    Logs logs_;
};

TEST_F(MetricsBaseTest, PostsTheGzippedPayloadToTheReportUrl)
{
    FakeBStats server;
    const auto metrics = make(server, true);
    ASSERT_TRUE(server.waitForRequest(10s));

    const auto request = server.first();
    EXPECT_EQ(request.request_line, "POST /api/v2/data/test HTTP/1.1\r");
    EXPECT_EQ(request.header("Accept"), "application/json");
    EXPECT_EQ(request.header("Connection"), "close");
    EXPECT_EQ(request.header("Content-Encoding"), "gzip");
    EXPECT_EQ(request.header("Content-Length"), std::to_string(request.body.size()));
    EXPECT_EQ(request.header("Content-Type"), "application/json");
    EXPECT_EQ(request.header("User-Agent"), "Metrics-Service/1");
    EXPECT_EQ(
        gunzip(request.body),
        R"({"osName":"Test","service":{"pluginVersion":"1","id":42,"customCharts":[{"chartId":"chart","data":{"value":"value"}}]},"serverUUID":"00000000-0000-4000-8000-000000000000","metricsVersion":"3.0.3"})");
}

TEST_F(MetricsBaseTest, SendsNothingWhenDisabled)
{
    FakeBStats server;
    const auto metrics = make(server, false);
    EXPECT_FALSE(server.waitForRequest(300ms));
}

TEST_F(MetricsBaseTest, SendsNothingWhenTheServiceIsDisabled)
{
    FakeBStats server;
    const auto metrics = make(server, true, false, false, false, 0ms, [] { return false; });
    EXPECT_FALSE(server.waitForRequest(300ms));
}

TEST_F(MetricsBaseTest, WaitsForTheDelay)
{
    FakeBStats server;
    const auto metrics = make(server, true, false, false, false, 1h);
    EXPECT_FALSE(server.waitForRequest(300ms));
}

TEST_F(MetricsBaseTest, ShutdownStopsTheSchedule)
{
    FakeBStats server;
    auto metrics = make(server, true, false, false, false, 400ms);
    metrics->shutdown();
    EXPECT_FALSE(server.waitForRequest(1s));
    metrics.reset();
}

TEST_F(MetricsBaseTest, LogsTheSentDataAndTheResponseWhenAsked)
{
    FakeBStats server(200, "{\"ok\":true}");
    const auto metrics = make(server, true, false, true, true);
    ASSERT_TRUE(server.waitForRequest(10s));
    for (int i = 0; i < 100; ++i) {
        {
            const std::scoped_lock lock(logs_.mutex);
            if (logs_.infos.size() == 2) {
                break;
            }
        }
        std::this_thread::sleep_for(50ms);
    }
    const std::scoped_lock lock(logs_.mutex);
    ASSERT_EQ(logs_.infos.size(), 2U);
    EXPECT_TRUE(logs_.infos[0].starts_with("Sent bStats metrics data: {\"osName\":\"Test\""));
    EXPECT_EQ(logs_.infos[1], "Sent data to bStats and received response: {\"ok\":true}");
}

TEST_F(MetricsBaseTest, LogsAnErrorStatusWhenAsked)
{
    FakeBStats server(500);
    const auto metrics = make(server, true, true);
    ASSERT_TRUE(server.waitForRequest(10s));
    for (int i = 0; i < 100; ++i) {
        {
            const std::scoped_lock lock(logs_.mutex);
            if (!logs_.errors.empty()) {
                break;
            }
        }
        std::this_thread::sleep_for(50ms);
    }
    const std::scoped_lock lock(logs_.mutex);
    ASSERT_FALSE(logs_.errors.empty());
    EXPECT_TRUE(logs_.errors[0].starts_with("Could not submit bStats metrics data: "));
    EXPECT_TRUE(logs_.infos.empty());
}

}  // namespace
}  // namespace endgate
