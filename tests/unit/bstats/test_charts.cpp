#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <glaze/json.hpp>
#include <gtest/gtest.h>

#include "endgate/bstats/charts.h"

namespace endgate {
namespace {

struct Logged {
    std::vector<std::string> messages;

    [[nodiscard]] ErrorLogger logger()
    {
        return [this](const std::string &message, const std::exception &error) {
            messages.push_back(message + ": " + error.what());
        };
    }
};

std::string requestOf(const CustomChart &chart)
{
    Logged logged;
    const auto request = chart.getRequestJsonObject(logged.logger(), true);
    return request ? glz::write_json(*request).value_or("unwritable") : "skipped";
}

using Pie = std::map<std::string, int>;
using Drilldown = std::map<std::string, std::map<std::string, int>>;

TEST(Charts, SimplePieSendsItsValue)
{
    const SimplePie chart("version", [] { return "1.2"; });
    EXPECT_EQ(requestOf(chart), R"({"chartId":"version","data":{"value":"1.2"}})");
}

TEST(Charts, SimplePieSkipsAnEmptyValue)
{
    const SimplePie chart("version", [] { return ""; });
    EXPECT_EQ(requestOf(chart), "skipped");
}

TEST(Charts, SimplePieEscapesItsValue)
{
    const SimplePie chart("version", [] { return "a\"b\\c\nd"; });
    EXPECT_EQ(requestOf(chart), R"json({"chartId":"version","data":{"value":"a\"b\\c\nd"}})json");
}

TEST(Charts, SingleLineChartSendsItsValue)
{
        const SingleLineChart chart("players", [] { return 7; });
        EXPECT_EQ(requestOf(chart), R"({"chartId":"players","data":{"value":7}})");
}

TEST(Charts, SingleLineChartSkipsZero)
{
        const SingleLineChart chart("players", [] { return 0; });
        EXPECT_EQ(requestOf(chart), "skipped");
}

TEST(Charts, AdvancedPieDropsZeroEntries)
{
        const AdvancedPie chart("pie", [] { return Pie{{"a", 2}, {"b", 0}, {"c", 1}}; });
        EXPECT_EQ(requestOf(chart), R"({"chartId":"pie","data":{"values":{"a":2,"c":1}}})");
}

TEST(Charts, AdvancedPieSkipsWhenNothingIsLeft)
{
        const AdvancedPie empty("pie", [] { return Pie{}; });
        const AdvancedPie zeros("pie", [] { return Pie{{"a", 0}}; });
        EXPECT_EQ(requestOf(empty), "skipped");
        EXPECT_EQ(requestOf(zeros), "skipped");
}

TEST(Charts, MultiLineChartDropsZeroEntries)
{
        const MultiLineChart chart("lines", [] { return Pie{{"a", 0}, {"b", 4}}; });
        EXPECT_EQ(requestOf(chart), R"({"chartId":"lines","data":{"values":{"b":4}}})");
}

TEST(Charts, DrilldownPieNestsValues)
{
        const DrilldownPie chart("os", [] { return Drilldown{{"Windows", {{"Windows 11 (10.0.26200)", 1}}}}; });
        EXPECT_EQ(requestOf(chart),
                  R"json({"chartId":"os","data":{"values":{"Windows":{"Windows 11 (10.0.26200)":1}}}})json");
}

TEST(Charts, DrilldownPieDropsEntriesWithoutValues)
{
        const DrilldownPie chart("os", [] { return Drilldown{{"Empty", {}}, {"Linux", {{"6.8", 1}}}}; });
        EXPECT_EQ(requestOf(chart), R"({"chartId":"os","data":{"values":{"Linux":{"6.8":1}}}})");
}

TEST(Charts, DrilldownPieSkipsWhenNothingIsLeft)
{
        const DrilldownPie empty("os", [] { return Drilldown{}; });
        const DrilldownPie hollow("os", [] { return Drilldown{{"Empty", {}}}; });
        EXPECT_EQ(requestOf(empty), "skipped");
        EXPECT_EQ(requestOf(hollow), "skipped");
}

TEST(Charts, SimpleBarChartWrapsEachValueInAnArray)
{
        const SimpleBarChart chart("bars", [] { return Pie{{"a", 1}, {"b", 0}}; });
        EXPECT_EQ(requestOf(chart), R"({"chartId":"bars","data":{"values":{"a":[1],"b":[0]}}})");
}

TEST(Charts, AdvancedBarChartDropsEmptyArrays)
{
        const AdvancedBarChart chart("bars",
                                     [] { return std::map<std::string, std::vector<int>>{{"a", {1, 2}}, {"b", {}}}; });
        EXPECT_EQ(requestOf(chart), R"({"chartId":"bars","data":{"values":{"a":[1,2]}}})");
}

TEST(Charts, AChartThatThrowsIsSkippedAndLogged)
{
        const SingleLineChart chart("players", []() -> int { throw std::runtime_error("boom"); });
        Logged logged;
        EXPECT_FALSE(chart.getRequestJsonObject(logged.logger(), true));
        ASSERT_EQ(logged.messages.size(), 1U);
        EXPECT_EQ(logged.messages.front(), "Failed to get data for custom chart with id players: boom");
}

TEST(Charts, AChartThatThrowsIsNotLoggedWhenErrorsAreOff)
{
        const SingleLineChart chart("players", []() -> int { throw std::runtime_error("boom"); });
        Logged logged;
        EXPECT_FALSE(chart.getRequestJsonObject(logged.logger(), false));
        EXPECT_TRUE(logged.messages.empty());
}

}  // namespace
}  // namespace endgate
