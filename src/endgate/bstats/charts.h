#pragma once

#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glaze/json/generic.hpp>

namespace endgate {

/**
 * A JSON value that keeps its fields in the order they were added.
 *
 * @see org.bstats.json.JsonObjectBuilder
 */
using JsonObject = glz::generic;

/**
 * Accepts a log message and the error that caused it.
 *
 * @see java.util.function.BiConsumer
 */
using ErrorLogger = std::function<void(const std::string &, const std::exception &)>;

/**
 * @see org.bstats.charts.CustomChart
 */
class CustomChart {
public:
    CustomChart(const CustomChart &) = delete;
    CustomChart &operator=(const CustomChart &) = delete;
    CustomChart(CustomChart &&) = delete;
    CustomChart &operator=(CustomChart &&) = delete;
    virtual ~CustomChart() = default;

    /**
     * @see org.bstats.charts.CustomChart#getRequestJsonObject
     */
    [[nodiscard]] std::optional<JsonObject> getRequestJsonObject(const ErrorLogger &error_logger,
                                                                 bool log_errors) const;

protected:
    explicit CustomChart(std::string chart_id) : chart_id_(std::move(chart_id)) {}

    /**
     * @see org.bstats.charts.CustomChart#getChartData
     */
    [[nodiscard]] virtual std::optional<JsonObject> getChartData() const = 0;

private:
    std::string chart_id_;
};

/**
 * @see org.bstats.charts.SimplePie
 */
class SimplePie : public CustomChart {
public:
    SimplePie(std::string chart_id, std::function<std::string()> callable);

protected:
    [[nodiscard]] std::optional<JsonObject> getChartData() const override;

private:
    std::function<std::string()> callable_;
};

/**
 * @see org.bstats.charts.SingleLineChart
 */
class SingleLineChart : public CustomChart {
public:
    SingleLineChart(std::string chart_id, std::function<int()> callable);

protected:
    [[nodiscard]] std::optional<JsonObject> getChartData() const override;

private:
    std::function<int()> callable_;
};

/**
 * @see org.bstats.charts.MultiLineChart
 */
class MultiLineChart : public CustomChart {
public:
    MultiLineChart(std::string chart_id, std::function<std::map<std::string, int>()> callable);

protected:
    [[nodiscard]] std::optional<JsonObject> getChartData() const override;

private:
    std::function<std::map<std::string, int>()> callable_;
};

/**
 * @see org.bstats.charts.AdvancedPie
 */
class AdvancedPie : public CustomChart {
public:
    AdvancedPie(std::string chart_id, std::function<std::map<std::string, int>()> callable);

protected:
    [[nodiscard]] std::optional<JsonObject> getChartData() const override;

private:
    std::function<std::map<std::string, int>()> callable_;
};

/**
 * @see org.bstats.charts.DrilldownPie
 */
class DrilldownPie : public CustomChart {
public:
    DrilldownPie(std::string chart_id, std::function<std::map<std::string, std::map<std::string, int>>()> callable);

protected:
    [[nodiscard]] std::optional<JsonObject> getChartData() const override;

private:
    std::function<std::map<std::string, std::map<std::string, int>>()> callable_;
};

/**
 * @see org.bstats.charts.SimpleBarChart
 */
class SimpleBarChart : public CustomChart {
public:
    SimpleBarChart(std::string chart_id, std::function<std::map<std::string, int>()> callable);

protected:
    [[nodiscard]] std::optional<JsonObject> getChartData() const override;

private:
    std::function<std::map<std::string, int>()> callable_;
};

/**
 * @see org.bstats.charts.AdvancedBarChart
 */
class AdvancedBarChart : public CustomChart {
public:
    AdvancedBarChart(std::string chart_id, std::function<std::map<std::string, std::vector<int>>()> callable);

protected:
    [[nodiscard]] std::optional<JsonObject> getChartData() const override;

private:
    std::function<std::map<std::string, std::vector<int>>()> callable_;
};

}  // namespace endgate
