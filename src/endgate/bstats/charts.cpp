#include "endgate/bstats/charts.h"

#include <stdexcept>
#include <utility>

#include <glaze/json.hpp>

namespace endgate {

namespace {

struct ChartRequest {
    std::string chart_id;
    JsonObject data;

    struct glaze {
        using T = ChartRequest;
        static constexpr auto value = glz::object(  //
            "chartId", &T::chart_id,                //
            "data", &T::data);
    };
};

struct StringValue {
    std::string value;

    struct glaze {
        using T = StringValue;
        static constexpr auto value = glz::object("value", &T::value);
    };
};

struct IntValue {
    int value = 0;

    struct glaze {
        using T = IntValue;
        static constexpr auto value = glz::object("value", &T::value);
    };
};

struct Values {
    JsonObject values;

    struct glaze {
        using T = Values;
        static constexpr auto value = glz::object("values", &T::values);
    };
};

template <typename T>
JsonObject toJsonObject(const T &value)
{
    const auto json = glz::write_json(value);
    if (!json) {
        throw std::runtime_error(glz::format_error(json.error()));
    }
    JsonObject object;
    if (const auto error = glz::read_json(object, *json)) {
        throw std::runtime_error(glz::format_error(error, *json));
    }
    return object;
}

std::optional<JsonObject> valuesWithoutZeros(const std::map<std::string, int> &map)
{
    if (map.empty()) {
        return std::nullopt;
    }
    JsonObject values;
    bool all_skipped = true;
    for (const auto &[key, value] : map) {
        if (value == 0) {
            continue;
        }
        all_skipped = false;
        values[key] = value;
    }
    if (all_skipped) {
        return std::nullopt;
    }
    return toJsonObject(Values{.values = std::move(values)});
}

}  // namespace

std::optional<JsonObject> CustomChart::getRequestJsonObject(const ErrorLogger &error_logger, bool log_errors) const
{
    try {
        auto data = getChartData();
        if (!data) {
            return std::nullopt;
        }
        return toJsonObject(ChartRequest{.chart_id = chart_id_, .data = std::move(*data)});
    }
    catch (const std::exception &error) {
        if (log_errors) {
            error_logger("Failed to get data for custom chart with id " + chart_id_, error);
        }
        return std::nullopt;
    }
}

SimplePie::SimplePie(std::string chart_id, std::function<std::string()> callable)
    : CustomChart(std::move(chart_id)), callable_(std::move(callable))
{
}

std::optional<JsonObject> SimplePie::getChartData() const
{
    auto value = callable_();
    if (value.empty()) {
        return std::nullopt;
    }
    return toJsonObject(StringValue{.value = std::move(value)});
}

SingleLineChart::SingleLineChart(std::string chart_id, std::function<int()> callable)
    : CustomChart(std::move(chart_id)), callable_(std::move(callable))
{
}

std::optional<JsonObject> SingleLineChart::getChartData() const
{
    const auto value = callable_();
    if (value == 0) {
        return std::nullopt;
    }
    return toJsonObject(IntValue{.value = value});
}

MultiLineChart::MultiLineChart(std::string chart_id, std::function<std::map<std::string, int>()> callable)
    : CustomChart(std::move(chart_id)), callable_(std::move(callable))
{
}

std::optional<JsonObject> MultiLineChart::getChartData() const
{
    return valuesWithoutZeros(callable_());
}

AdvancedPie::AdvancedPie(std::string chart_id, std::function<std::map<std::string, int>()> callable)
    : CustomChart(std::move(chart_id)), callable_(std::move(callable))
{
}

std::optional<JsonObject> AdvancedPie::getChartData() const
{
    return valuesWithoutZeros(callable_());
}

DrilldownPie::DrilldownPie(std::string chart_id,
                           std::function<std::map<std::string, std::map<std::string, int>>()> callable)
    : CustomChart(std::move(chart_id)), callable_(std::move(callable))
{
}

std::optional<JsonObject> DrilldownPie::getChartData() const
{
    const auto map = callable_();
    if (map.empty()) {
        return std::nullopt;
    }
    JsonObject values;
    bool really_all_skipped = true;
    for (const auto &[key, entry] : map) {
        if (entry.empty()) {
            continue;
        }
        really_all_skipped = false;
        for (const auto &[name, value] : entry) {
            values[key][name] = value;
        }
    }
    if (really_all_skipped) {
        return std::nullopt;
    }
    return toJsonObject(Values{.values = std::move(values)});
}

SimpleBarChart::SimpleBarChart(std::string chart_id, std::function<std::map<std::string, int>()> callable)
    : CustomChart(std::move(chart_id)), callable_(std::move(callable))
{
}

std::optional<JsonObject> SimpleBarChart::getChartData() const
{
    const auto map = callable_();
    if (map.empty()) {
        return std::nullopt;
    }
    JsonObject values;
    for (const auto &[key, value] : map) {
        values[key] = toJsonObject(std::vector<int>{value});
    }
    return toJsonObject(Values{.values = std::move(values)});
}

AdvancedBarChart::AdvancedBarChart(std::string chart_id,
                                   std::function<std::map<std::string, std::vector<int>>()> callable)
    : CustomChart(std::move(chart_id)), callable_(std::move(callable))
{
}

std::optional<JsonObject> AdvancedBarChart::getChartData() const
{
    const auto map = callable_();
    if (map.empty()) {
        return std::nullopt;
    }
    JsonObject values;
    bool all_skipped = true;
    for (const auto &[key, entry] : map) {
        if (entry.empty()) {
            continue;
        }
        all_skipped = false;
        values[key] = toJsonObject(entry);
    }
    if (all_skipped) {
        return std::nullopt;
    }
    return toJsonObject(Values{.values = std::move(values)});
}

}  // namespace endgate
