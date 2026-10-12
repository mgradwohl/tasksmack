/// @file test_HistoryCsv.cpp
/// @brief UI::HistoryCsv (#808): a history chart's series as CSV -- header, rows by sample time,
/// gaps, series sampled at different times, field escaping and the timestamps.

#include "UI/HistoryCsv.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <limits>
#include <string>

namespace UI::HistoryCsv
{
namespace
{

/// 2026-10-12 03:04:05 UTC.
[[nodiscard]] std::chrono::system_clock::time_point now()
{
    return std::chrono::sys_days{std::chrono::year{2026} / 10 / 12} + std::chrono::hours{3} + std::chrono::minutes{4} +
           std::chrono::seconds{5};
}

TEST(HistoryCsvTest, OneRowPerSampleOldestFirstWithTheTimeAndSecondsAgo)
{
    const std::array series{Series{.label = "CPU Total", .x = {-2.0, -1.0, 0.0}, .y = {10.0, 12.5, 15.0}}};
    EXPECT_EQ(build(series, now()),
              "Time (UTC),Seconds ago,CPU Total\r\n"
              "2026-10-12T03:04:03.000Z,2.000,10\r\n"
              "2026-10-12T03:04:04.000Z,1.000,12.5\r\n"
              "2026-10-12T03:04:05.000Z,0.000,15\r\n");
}

TEST(HistoryCsvTest, SeriesSharingTheirTimesShareRows)
{
    const std::array series{Series{.label = "Used", .x = {-1.0, 0.0}, .y = {1.0, 2.0}},
                            Series{.label = "Cached", .x = {-1.0, 0.0}, .y = {3.0, 4.0}}};
    EXPECT_EQ(build(series, now()),
              "Time (UTC),Seconds ago,Used,Cached\r\n"
              "2026-10-12T03:04:04.000Z,1.000,1,3\r\n"
              "2026-10-12T03:04:05.000Z,0.000,2,4\r\n");
}

TEST(HistoryCsvTest, ASeriesWithoutASampleAtARowsTimeLeavesItsCellEmpty)
{
    const std::array series{Series{.label = "A", .x = {-1.5, -0.5}, .y = {1.0, 2.0}}, Series{.label = "B", .x = {-1.0}, .y = {7.0}}};
    EXPECT_EQ(build(series, now()),
              "Time (UTC),Seconds ago,A,B\r\n"
              "2026-10-12T03:04:03.500Z,1.500,1,\r\n"
              "2026-10-12T03:04:04.000Z,1.000,,7\r\n"
              "2026-10-12T03:04:04.500Z,0.500,2,\r\n");
}

TEST(HistoryCsvTest, AGapIsAnEmptyCellAndANonFiniteTimeIsNoRow)
{
    constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
    const std::array series{Series{.label = "Rate", .x = {-2.0, NaN, 0.0}, .y = {5.0, 6.0, NaN}}};
    EXPECT_EQ(build(series, now()),
              "Time (UTC),Seconds ago,Rate\r\n"
              "2026-10-12T03:04:03.000Z,2.000,5\r\n"
              "2026-10-12T03:04:05.000Z,0.000,\r\n");
}

TEST(HistoryCsvTest, MismatchedLengthsUseTheShorter)
{
    const std::array series{Series{.label = "S", .x = {-1.0, 0.0}, .y = {9.0}}};
    EXPECT_EQ(build(series, now()), "Time (UTC),Seconds ago,S\r\n2026-10-12T03:04:04.000Z,1.000,9\r\n");
}

TEST(HistoryCsvTest, HeadersAreEscapedAndAnUnnamedSeriesIsValue)
{
    EXPECT_EQ(escapeField("plain"), "plain");
    EXPECT_EQ(escapeField("a,b"), "\"a,b\"");
    EXPECT_EQ(escapeField("say \"hi\""), "\"say \"\"hi\"\"\"");
    EXPECT_EQ(escapeField("two\nlines"), "\"two\nlines\"");

    const std::array series{Series{.label = "Read, Write", .x = {0.0}, .y = {1.0}}, Series{.label = "", .x = {0.0}, .y = {2.0}}};
    EXPECT_EQ(build(series, now()), "Time (UTC),Seconds ago,\"Read, Write\",Value\r\n2026-10-12T03:04:05.000Z,0.000,1,2\r\n");
}

TEST(HistoryCsvTest, NoSeriesIsJustTheHeader)
{
    EXPECT_EQ(build({}, now()), "Time (UTC),Seconds ago\r\n");
}

TEST(HistoryCsvTest, ValuesKeepTheirPrecision)
{
    const std::array series{Series{.label = "Bytes", .x = {0.0}, .y = {123456789012.0}}, Series{.label = "Pct", .x = {0.0}, .y = {0.1}}};
    EXPECT_EQ(build(series, now()), "Time (UTC),Seconds ago,Bytes,Pct\r\n2026-10-12T03:04:05.000Z,0.000,123456789012,0.1\r\n");
}

TEST(HistoryCsvTest, TimesAreUtcToTheMillisecond)
{
    EXPECT_EQ(formatUtc(now() + std::chrono::milliseconds{250}), "2026-10-12T03:04:05.250Z");
}

} // namespace
} // namespace UI::HistoryCsv
