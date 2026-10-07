/// @file test_ProcessDetailsHistory.cpp
/// @brief Tests for Process Details' per-process history (#1179): every series appended, trimmed and
/// cleared with the time axis, the gap point (#1098), the trim anchor (#1016), the reset on a new
/// selection or a reused PID, and the newest point held to "now" by the charts (#1016, #1147).

#include "App/Panels/ProcessDetailsHistory.h"
#include "App/Panels/ProcessDetailsLayout.h"
#include "App/Panels/ProcessDetailsPanel_HistoryHelpers.h"
#include "Domain/ProcessSnapshot.h"
#include "UI/ChartWidgets.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace App::Detail
{
namespace
{

/// A point whose value for series i is base + i, so each series' values can be told apart.
[[nodiscard]] ProcessHistoryPoint distinctPoint(double base)
{
    ProcessHistoryPoint point;
    for (std::size_t i = 0; i < PROCESS_SERIES_COUNT; ++i)
    {
        point.*PROCESS_SERIES_FIELDS[i] = base + static_cast<double>(i);
    }
    return point;
}

[[nodiscard]] ProcessSeries seriesAt(std::size_t index)
{
    return static_cast<ProcessSeries>(index);
}

/// Every series has exactly one value per timestamp.
void expectAligned(const ProcessDetailsHistory& history)
{
    EXPECT_EQ(history.timestamps().size(), history.size());
    for (std::size_t i = 0; i < PROCESS_SERIES_COUNT; ++i)
    {
        SCOPED_TRACE("series " + std::to_string(i));
        EXPECT_EQ(history.series(seriesAt(i)).size(), history.size());
    }
}

/// A history of one point per second from @p first to @p last inclusive, each with distinctPoint(t * 100).
[[nodiscard]] ProcessDetailsHistory everySecond(int first, int last)
{
    ProcessDetailsHistory history;
    for (int t = first; t <= last; ++t)
    {
        history.append(static_cast<double>(t), distinctPoint(t * 100.0), false);
    }
    return history;
}

[[nodiscard]] std::vector<double> toVector(std::span<const double> values)
{
    return {values.begin(), values.end()};
}

// ========== append ==========

TEST(ProcessDetailsHistoryTest, StartsEmpty)
{
    const ProcessDetailsHistory history;
    EXPECT_TRUE(history.empty());
    EXPECT_EQ(history.size(), 0U);
    expectAligned(history);
}

TEST(ProcessDetailsHistoryTest, AppendStoresEachNamedValueInItsOwnSeries)
{
    ProcessDetailsHistory history;
    history.append(1.0, distinctPoint(10.0), false);
    history.append(2.0, distinctPoint(20.0), false);

    ASSERT_EQ(history.size(), 2U);
    expectAligned(history);
    EXPECT_EQ(toVector(history.timestamps()), (std::vector<double>{1.0, 2.0}));
    EXPECT_DOUBLE_EQ(history.newestTimeSeconds(), 2.0);
    for (std::size_t i = 0; i < PROCESS_SERIES_COUNT; ++i)
    {
        SCOPED_TRACE("series " + std::to_string(i));
        const auto values = history.series(seriesAt(i));
        EXPECT_DOUBLE_EQ(values[0], 10.0 + static_cast<double>(i));
        EXPECT_DOUBLE_EQ(values[1], 20.0 + static_cast<double>(i));
    }
}

TEST(ProcessDetailsHistoryTest, SeriesAreInTheOrderTheChartsReadThem)
{
    // The charts read series by name; a field wired to the wrong series would chart, say, user CPU
    // as system CPU.
    ProcessHistoryPoint point;
    point.cpuTotal = 1.0;
    point.cpuUser = 2.0;
    point.cpuSystem = 3.0;
    point.memoryUsed = 4.0;
    point.memoryShared = 5.0;
    point.virtualBytes = 6.0;
    point.threads = 7.0;
    point.handles = 8.0;
    point.pageFaults = 9.0;
    point.ioRead = 10.0;
    point.ioWrite = 11.0;
    point.netSent = 12.0;
    point.netReceived = 13.0;
    point.power = 14.0;
    point.gpuUtil = 15.0;
    point.gpuMemory = 16.0;
    point.gdiObjects = 17.0;

    ProcessDetailsHistory history;
    history.append(0.0, point, false);

    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::CpuTotal)[0], 1.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::CpuUser)[0], 2.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::CpuSystem)[0], 3.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::MemoryUsed)[0], 4.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::MemoryShared)[0], 5.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::Virtual)[0], 6.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::Threads)[0], 7.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::Handles)[0], 8.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::PageFaults)[0], 9.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::IoRead)[0], 10.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::IoWrite)[0], 11.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::NetSent)[0], 12.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::NetReceived)[0], 13.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::Power)[0], 14.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::GpuUtil)[0], 15.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::GpuMemory)[0], 16.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::GdiObjects)[0], 17.0);
}

// ========== Gap point (#1098) ==========

TEST(ProcessDetailsHistoryTest, GapBeforeAddsAMidpointOfNaNInEverySeries)
{
    ProcessDetailsHistory history;
    history.append(10.0, distinctPoint(1.0), false);
    history.append(14.0, distinctPoint(2.0), true);

    ASSERT_EQ(history.size(), 3U);
    expectAligned(history);
    EXPECT_EQ(toVector(history.timestamps()), (std::vector<double>{10.0, 12.0, 14.0}));
    for (std::size_t i = 0; i < PROCESS_SERIES_COUNT; ++i)
    {
        SCOPED_TRACE("series " + std::to_string(i));
        const auto values = history.series(seriesAt(i));
        EXPECT_DOUBLE_EQ(values[0], 1.0 + static_cast<double>(i));
        EXPECT_TRUE(std::isnan(values[1]));
        EXPECT_DOUBLE_EQ(values[2], 2.0 + static_cast<double>(i)); // The newest point is the reading, not the gap
    }
}

TEST(ProcessDetailsHistoryTest, GapBeforeTheFirstPointAddsNoGap)
{
    // Nothing to draw a line from: the first sample after a selection never starts with a gap.
    ProcessDetailsHistory history;
    history.append(5.0, distinctPoint(1.0), true);

    ASSERT_EQ(history.size(), 1U);
    expectAligned(history);
    EXPECT_DOUBLE_EQ(history.timestamps()[0], 5.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::CpuTotal)[0], 1.0);
}

TEST(ProcessDetailsHistoryTest, GapBeforeAPointNotAfterTheNewestAddsNoGap)
{
    ProcessDetailsHistory history;
    history.append(5.0, distinctPoint(1.0), false);
    history.append(5.0, distinctPoint(2.0), true); // Same time: no room for a midpoint
    history.append(4.0, distinctPoint(3.0), true); // Earlier time: likewise

    ASSERT_EQ(history.size(), 3U);
    expectAligned(history);
    EXPECT_EQ(toVector(history.timestamps()), (std::vector<double>{5.0, 5.0, 4.0}));
    for (std::size_t i = 0; i < PROCESS_SERIES_COUNT; ++i)
    {
        SCOPED_TRACE("series " + std::to_string(i));
        for (const double value : history.series(seriesAt(i)))
        {
            EXPECT_FALSE(std::isnan(value));
        }
    }
}

// ========== Trim to the history window (#1016, #1145) ==========

TEST(ProcessDetailsHistoryTest, TrimKeepsTheWindowPlusTheNewestPointBeforeIt)
{
    // Points 0..20 s, a 10 s window: the cutoff is 10 s, and the point at 9 s stays as the anchor the
    // lines run off the left edge from (#1016).
    ProcessDetailsHistory history = everySecond(0, 20);
    history.trimToWindow(10.0);

    ASSERT_EQ(history.size(), 12U);
    expectAligned(history);
    EXPECT_DOUBLE_EQ(history.timestamps().front(), 9.0);
    EXPECT_DOUBLE_EQ(history.newestTimeSeconds(), 20.0);
    for (std::size_t i = 0; i < PROCESS_SERIES_COUNT; ++i)
    {
        SCOPED_TRACE("series " + std::to_string(i));
        const auto values = history.series(seriesAt(i));
        // Every series drops the same oldest points as the axis
        EXPECT_DOUBLE_EQ(values.front(), 900.0 + static_cast<double>(i));
        EXPECT_DOUBLE_EQ(values.back(), 2000.0 + static_cast<double>(i));
    }
}

TEST(ProcessDetailsHistoryTest, TrimWithinTheWindowKeepsEverything)
{
    ProcessDetailsHistory history = everySecond(0, 5);
    history.trimToWindow(300.0);

    EXPECT_EQ(history.size(), 6U);
    expectAligned(history);
}

TEST(ProcessDetailsHistoryTest, TrimDropsTheAnchorWhenItIsFurtherBackThanTheWindow)
{
    // The anchor is kept only when the step from it to the next point is no longer than the window
    // (Domain::HistoryUtils::keepTrimAnchor): otherwise it would draw a line across a stall.
    ProcessDetailsHistory history = everySecond(0, 2);
    history.append(15.0, distinctPoint(1500.0), false);
    history.trimToWindow(10.0);

    ASSERT_EQ(history.size(), 1U);
    expectAligned(history);
    EXPECT_DOUBLE_EQ(history.timestamps()[0], 15.0);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::CpuTotal)[0], 1500.0);
}

TEST(ProcessDetailsHistoryTest, TrimToAZeroWindowKeepsOnlyTheNewestPoint)
{
    ProcessDetailsHistory history = everySecond(0, 4);
    history.trimToWindow(0.0);

    ASSERT_EQ(history.size(), 1U);
    expectAligned(history);
    EXPECT_DOUBLE_EQ(history.newestTimeSeconds(), 4.0);
}

TEST(ProcessDetailsHistoryTest, ShorterWindowTrimsAtOnce)
{
    // A history-window change is applied when it arrives, not at the next sample (#1145).
    ProcessDetailsHistory history = everySecond(0, 60);
    history.trimToWindow(300.0);
    ASSERT_EQ(history.size(), 61U);

    history.trimToWindow(5.0);
    EXPECT_EQ(history.size(), 7U); // 54 (anchor) .. 60
    EXPECT_DOUBLE_EQ(history.timestamps().front(), 54.0);
    expectAligned(history);
}

TEST(ProcessDetailsHistoryTest, TrimKeepsGapPointsAlignedInEverySeries)
{
    ProcessDetailsHistory history = everySecond(0, 2);
    history.append(14.0, distinctPoint(1400.0), true); // Gap point at 8 s
    history.trimToWindow(10.0);                        // Cutoff 4 s: 8 s is in, 2 s is its anchor

    EXPECT_EQ(toVector(history.timestamps()), (std::vector<double>{2.0, 8.0, 14.0}));
    expectAligned(history);
    for (std::size_t i = 0; i < PROCESS_SERIES_COUNT; ++i)
    {
        SCOPED_TRACE("series " + std::to_string(i));
        const auto values = history.series(seriesAt(i));
        EXPECT_DOUBLE_EQ(values[0], 200.0 + static_cast<double>(i));
        EXPECT_TRUE(std::isnan(values[1]));
        EXPECT_DOUBLE_EQ(values[2], 1400.0 + static_cast<double>(i));
    }
}

// A trim cannot leave the buffers at different lengths partway through (#1442 review): it never throws.
static_assert(noexcept(std::declval<ProcessDetailsHistory&>().trimToWindow(1.0)));
static_assert(noexcept(std::declval<ProcessDetailsHistory&>().clear()));

TEST(ProcessDetailsHistoryTest, TrimOfAnEmptyHistoryDoesNothing)
{
    ProcessDetailsHistory history;
    history.trimToWindow(10.0);
    EXPECT_TRUE(history.empty());
    expectAligned(history);
}

// ========== Reset on a new selection, including a reused PID ==========

TEST(ProcessDetailsHistoryTest, ClearEmptiesTheAxisAndEverySeries)
{
    ProcessDetailsHistory history = everySecond(0, 9);
    history.clear();

    EXPECT_TRUE(history.empty());
    expectAligned(history);
}

TEST(ProcessDetailsHistoryTest, AReusedPidStartsAFreshHistory)
{
    // The panel resets on any selection snapshotIsSelectedProcess() calls a different process: a
    // different key under the same PID is a reused PID, so its history must not continue the old one's.
    constexpr std::int32_t pid = 42;
    constexpr std::uint64_t oldKey = 1;
    constexpr std::uint64_t newKey = 2;

    ProcessDetailsHistory history = everySecond(0, 9);
    ASSERT_FALSE(ProcessDetailsLayout::snapshotIsSelectedProcess(pid, oldKey, pid, newKey));
    history.clear();

    // The new process's first sample is the first point, with no gap from the old history.
    history.append(20.0, distinctPoint(7.0), true);
    ASSERT_EQ(history.size(), 1U);
    expectAligned(history);
    EXPECT_DOUBLE_EQ(history.timestamps()[0], 20.0);
}

TEST(ProcessDetailsHistoryTest, ReselectingTheSameProcessKeepsItsHistory)
{
    // An unknown key on either side is not a different process: re-selecting what is shown keeps it.
    constexpr std::int32_t pid = 42;
    EXPECT_TRUE(ProcessDetailsLayout::snapshotIsSelectedProcess(pid, 1, pid, 0));
    EXPECT_TRUE(ProcessDetailsLayout::snapshotIsSelectedProcess(pid, 0, pid, 1));
    EXPECT_TRUE(ProcessDetailsLayout::snapshotIsSelectedProcess(pid, 1, pid, 1));
}

TEST(ProcessDetailsHistoryTest, SamplesOfAProcessThatReusedThePidAreNotAppended)
{
    // Between a PID's reuse and the reselection, samples of the new process are not the selected one's.
    constexpr std::int32_t pid = 42;
    std::uint64_t selectedKey = 1;
    SampleIntake intake;
    ProcessDetailsHistory history;

    const auto sample = [](std::uint64_t version, double time, std::uint64_t key)
    {
        Domain::ProcessSnapshot snapshot;
        snapshot.pid = pid;
        snapshot.uniqueKey = key;
        snapshot.cpuPercent = static_cast<double>(key);
        return Domain::ProcessSample{.snapshot = std::make_shared<const Domain::ProcessSnapshot>(snapshot),
                                     .version = version,
                                     .sampleTimeSeconds = time,
                                     .ioCountersSupported = true,
                                     .networkCountersSupported = true,
                                     .gpuPerProcessSupported = true,
                                     .gpuUtilizationSupported = true,
                                     .gpuReadFailed = false};
    };
    const std::vector<Domain::ProcessSample> samples{sample(1, 1.0, 1), sample(2, 2.0, 2), sample(3, 3.0, 2)};
    takeSamples(samples,
                pid,
                selectedKey,
                intake,
                [&history](const Domain::ProcessSample& s, bool gapBefore)
                { history.append(s.sampleTimeSeconds, historyPointFrom(*s.snapshot, rateReadings(s)), gapBefore); });

    ASSERT_EQ(history.size(), 1U);
    expectAligned(history);
    EXPECT_DOUBLE_EQ(history.series(ProcessSeries::CpuTotal)[0], 1.0);
    EXPECT_FALSE(intake.present);
}

// ========== Held to now (#1016, #1147) ==========

TEST(ProcessDetailsHistoryTest, NewestPointIsHeldToNowByTheCharts)
{
    // The charts draw each series to x = 0 by repeating its newest value (holdLastValuesToNow), which
    // needs the newest point to be the latest reading -- never a gap point, and never trimmed away.
    ProcessDetailsHistory history = everySecond(0, 9);
    history.append(14.0, distinctPoint(1400.0), true); // A gap point goes before it, not after
    history.trimToWindow(10.0);

    constexpr double nowSeconds = 14.5;
    std::vector<double> x;
    UI::Widgets::fillTimeAxis(x, history.timestamps(), history.size(), nowSeconds);
    const auto user = history.series(ProcessSeries::CpuUser);
    const auto total = history.series(ProcessSeries::CpuTotal);
    std::vector<double> userValues(user.begin(), user.end());
    std::vector<double> totalValues(total.begin(), total.end());

    UI::Widgets::holdLastValuesToNow(x, {&userValues, &totalValues}, UI::Widgets::maxHoldSecondsForAxis(std::span<const double>(x)));

    ASSERT_EQ(x.size(), history.size() + 1);
    EXPECT_DOUBLE_EQ(x.back(), 0.0);
    EXPECT_DOUBLE_EQ(userValues.back(), 1401.0);
    EXPECT_DOUBLE_EQ(totalValues.back(), 1400.0);
}

// ========== historyPointFrom ==========

TEST(ProcessDetailsHistoryTest, HistoryPointCarriesReadingsAndGapsWhereUnread)
{
    Domain::ProcessSnapshot snapshot;
    snapshot.cpuPercent = 12.5;
    snapshot.cpuUserPercent = 10.0;
    snapshot.cpuSystemPercent = 2.5;
    snapshot.memoryBytes = 1000;
    snapshot.sharedBytes = 200;
    snapshot.virtualBytes = 5000;
    snapshot.threadCount = 8;
    snapshot.handleCount = 30;
    snapshot.handleCountAvailable = true;
    snapshot.pageFaultsPerSec = 4.0;
    snapshot.ioReadBytesPerSec = 100.0;
    snapshot.ioWriteBytesPerSec = 50.0;
    snapshot.netSentBytesPerSec = 10.0;
    snapshot.netReceivedBytesPerSec = 20.0;
    snapshot.powerWatts = 1.5;
    snapshot.gpuUtilPercent = 33.0;
    snapshot.gpuMemoryBytes = 4096;
    snapshot.gdiObjectCount = 77;

    const ProcessHistoryPoint all =
        historyPointFrom(snapshot, {.io = true, .network = true, .gpuPerProcess = true, .gpuUtilization = true, .gpuSupported = true});
    EXPECT_DOUBLE_EQ(all.cpuTotal, 12.5);
    EXPECT_DOUBLE_EQ(all.cpuUser, 10.0);
    EXPECT_DOUBLE_EQ(all.cpuSystem, 2.5);
    EXPECT_DOUBLE_EQ(all.memoryUsed, 1000.0);
    EXPECT_DOUBLE_EQ(all.memoryShared, 200.0);
    EXPECT_DOUBLE_EQ(all.virtualBytes, 5000.0);
    EXPECT_DOUBLE_EQ(all.threads, 8.0);
    EXPECT_DOUBLE_EQ(all.handles, 30.0);
    EXPECT_DOUBLE_EQ(all.pageFaults, 4.0);
    EXPECT_DOUBLE_EQ(all.ioRead, 100.0);
    EXPECT_DOUBLE_EQ(all.ioWrite, 50.0);
    EXPECT_DOUBLE_EQ(all.netSent, 10.0);
    EXPECT_DOUBLE_EQ(all.netReceived, 20.0);
    EXPECT_DOUBLE_EQ(all.power, 1.5);
    EXPECT_DOUBLE_EQ(all.gpuUtil, 33.0);
    EXPECT_DOUBLE_EQ(all.gpuMemory, 4096.0);
    EXPECT_DOUBLE_EQ(all.gdiObjects, 77.0);

    // Unread values are gaps (#1110, #1148, #1210), not measured-looking zeros.
    snapshot.handleCountAvailable = false;
    snapshot.gdiObjectCount.reset();
    const ProcessHistoryPoint none = historyPointFrom(snapshot, {});
    EXPECT_TRUE(std::isnan(none.handles));
    EXPECT_TRUE(std::isnan(none.ioRead));
    EXPECT_TRUE(std::isnan(none.ioWrite));
    EXPECT_TRUE(std::isnan(none.netSent));
    EXPECT_TRUE(std::isnan(none.netReceived));
    EXPECT_TRUE(std::isnan(none.gpuUtil));
    EXPECT_TRUE(std::isnan(none.gpuMemory));
    EXPECT_TRUE(std::isnan(none.gdiObjects));
    EXPECT_DOUBLE_EQ(none.cpuTotal, 12.5); // Always-read values stay readings

    // GPU memory without utilization (NVML on Linux): memory is a reading, utilization a gap.
    const ProcessHistoryPoint memoryOnly = historyPointFrom(snapshot, {.gpuPerProcess = true});
    EXPECT_DOUBLE_EQ(memoryOnly.gpuMemory, 4096.0);
    EXPECT_TRUE(std::isnan(memoryOnly.gpuUtil));
}

} // namespace
} // namespace App::Detail
