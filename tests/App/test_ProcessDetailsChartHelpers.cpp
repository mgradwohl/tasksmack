/// @file test_ProcessDetailsChartHelpers.cpp
/// @brief Tests for the pure pieces of Process Details' chart tabs (#1179, slice 5): the NowBar
/// columns, the value text, each chart's axis target, and the GPU and Network and I/O tabs'
/// empty-state decisions.

#include "App/Panels/ProcessDetailsChartHelpers.h"
#include "App/Panels/ProcessDetailsHistory.h"
#include "App/Panels/ProcessDetailsPanel_GpuHelpers.h"
#include "App/Panels/ProcessSmoothedUsage.h"
#include "Domain/ProcessSnapshot.h"
#include "UI/Format.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace App::Detail
{
namespace
{

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

// ========== NowBar columns ==========

TEST(ProcessDetailsChartHelpersTest, EachTabReservesTheMostBarsAnyOfItsChartsHas)
{
    // Overview: CPU has three bars (Total, User, System), Memory up to three (Memory, Shared,
    // Virtual), Resources three (Threads, FDs/Handles, Page Faults) plus GDI Objects on Windows.
#ifdef _WIN32
    EXPECT_EQ(PROCESS_OVERVIEW_NOW_BAR_COLUMNS, 4U);
#else
    EXPECT_EQ(PROCESS_OVERVIEW_NOW_BAR_COLUMNS, 3U);
#endif
    EXPECT_EQ(PROCESS_NETWORK_IO_NOW_BAR_COLUMNS, 2U); // Read/Write, Sent/Received
    EXPECT_EQ(PROCESS_GPU_NOW_BAR_COLUMNS, 1U);        // Utilization, Memory: one each
}

// ========== Value text ==========

TEST(ProcessDetailsChartHelpersTest, CountTextRoundsAReadingAndShowsNAForAGap)
{
    EXPECT_EQ(formatCountOrNA(12.4), UI::Format::formatIntLocalized(12LL));
    EXPECT_EQ(formatCountOrNA(12.6), UI::Format::formatIntLocalized(13LL));
    EXPECT_EQ(formatCountOrNA(NOT_A_NUMBER), "N/A");
    EXPECT_EQ(formatCountOrNA(std::numeric_limits<double>::infinity()), "N/A");
}

TEST(ProcessDetailsChartHelpersTest, CountBarTextIsNAWithoutAReading)
{
    EXPECT_EQ(countTextOrNA(true, 41.7), UI::Format::formatIntLocalized(42LL));
    EXPECT_EQ(countTextOrNA(false, 41.7), "N/A");
}

TEST(ProcessDetailsChartHelpersTest, BytesWithRamShareNamesBothAndClampsTheShare)
{
    const double bytes = 1024.0 * 1024.0;
    EXPECT_EQ(formatBytesWithRamShare(bytes, 0.5 / bytes),
              UI::Format::formatBytes(bytes) + " (" + UI::Format::percentOneDecimal(0.5) + " of RAM)");
    // A share past 100 % (a stale percent-per-byte) is held to 100 %, a negative one to 0 %.
    EXPECT_EQ(formatBytesWithRamShare(bytes, 1.0),
              UI::Format::formatBytes(bytes) + " (" + UI::Format::percentOneDecimal(100.0) + " of RAM)");
    EXPECT_EQ(formatBytesWithRamShare(bytes, -1.0),
              UI::Format::formatBytes(bytes) + " (" + UI::Format::percentOneDecimal(0.0) + " of RAM)");
    EXPECT_EQ(formatBytesWithRamShare(NOT_A_NUMBER, 1.0), "N/A");
}

TEST(ProcessDetailsChartHelpersTest, RateTextUsesTheRateUnitOrNA)
{
    const double rate = 3.5 * 1024.0 * 1024.0;
    EXPECT_EQ(rateTextOrNA(true, rate), UI::Format::formatBytesPerSecWithUnit(rate, UI::Format::unitForBytesPerSecond(rate)));
    EXPECT_EQ(rateTextOrNA(false, rate), "N/A");
}

TEST(ProcessDetailsChartHelpersTest, EnginesAreJoinedWithCommas)
{
    EXPECT_EQ(joinWithCommas(std::vector<std::string>{}), "");
    EXPECT_EQ(joinWithCommas(std::vector<std::string>{"3D"}), "3D");
    EXPECT_EQ(joinWithCommas(std::vector<std::string>{"3D", "Compute", "Copy"}), "3D, Compute, Copy");
}

TEST(ProcessDetailsChartHelpersTest, GpuMemoryKindsShowWhenTheTotalDoesNotAlreadyShowThem)
{
    EXPECT_FALSE(showsGpuMemoryKinds(4096, 4096, 0));   // All dedicated: the total says it
    EXPECT_TRUE(showsGpuMemoryKinds(6144, 4096, 2048)); // Shared memory mapped
    EXPECT_TRUE(showsGpuMemoryKinds(0, 4096, 0));       // Dedicated not what was counted
    EXPECT_FALSE(showsGpuMemoryKinds(0, 0, 0));
}

// ========== Axis targets ==========

/// A three-sample window at x = -2, -1, 0, with a fourth sample left of it (the trim anchor).
struct AxisFixture
{
    std::array<double, 4> x{-10.0, -2.0, -1.0, 0.0};
    double xMin = -5.0;
};

TEST(ProcessDetailsChartHelpersTest, CpuAxisCoversEverySeriesInTheWindowAndTheBars)
{
    const AxisFixture f;
    const std::array<double, 4> total{90.0, 10.0, 12.0, 11.0}; // 90 is left of the window
    const std::array<double, 4> user{0.0, 6.0, NOT_A_NUMBER, 7.0};
    const std::array<double, 4> system{0.0, 3.0, 14.0, 4.0};
    ProcessSmoothedUsage smoothed;
    EXPECT_DOUBLE_EQ(cpuAxisDataMax(f.x, f.xMin, total, user, system, smoothed), 14.0);
    smoothed.cpuUserPercent = 20.0; // A bar still easing down from a peak that left the window
    EXPECT_DOUBLE_EQ(cpuAxisDataMax(f.x, f.xMin, total, user, system, smoothed), 20.0);
}

TEST(ProcessDetailsChartHelpersTest, MemoryAxisCoversUsedAndSharedButNotThePeakOrVirtual)
{
    const AxisFixture f;
    const std::array<double, 4> used{900.0, 100.0, 120.0, 110.0};
    const std::array<double, 4> shared{0.0, 30.0, 40.0, 150.0};
    ProcessSmoothedUsage smoothed;
    smoothed.virtualBytes = 1.0e9; // Its own axis
    EXPECT_DOUBLE_EQ(memoryAxisDataMax(f.x, f.xMin, used, shared, smoothed), 150.0);
    // Shared not drawn (#1035): only Used counts, plus the bars.
    EXPECT_DOUBLE_EQ(memoryAxisDataMax(f.x, f.xMin, used, std::span<const double>{}, smoothed), 120.0);
    smoothed.memorySharedBytes = 500.0;
    EXPECT_DOUBLE_EQ(memoryAxisDataMax(f.x, f.xMin, used, std::span<const double>{}, smoothed), 500.0);
}

TEST(ProcessDetailsChartHelpersTest, VirtualAxisCoversItsSeriesAndBar)
{
    const AxisFixture f;
    const std::array<double, 4> virt{9000.0, 1000.0, NOT_A_NUMBER, 1200.0};
    ProcessSmoothedUsage smoothed;
    EXPECT_DOUBLE_EQ(virtualAxisDataMax(f.x, f.xMin, virt, smoothed), 1200.0);
    smoothed.virtualBytes = 2000.0;
    EXPECT_DOUBLE_EQ(virtualAxisDataMax(f.x, f.xMin, virt, smoothed), 2000.0);
}

TEST(ProcessDetailsChartHelpersTest, CountAxisLeavesOutAnUnreadHandleBar)
{
    const AxisFixture f;
    const std::array<double, 4> threads{99.0, 8.0, 9.0, 10.0};
    const std::array<double, 4> handles{0.0, 20.0, NOT_A_NUMBER, 25.0};
    ProcessSmoothedUsage smoothed;
    smoothed.threadCount = 10.0;
    smoothed.handleCount = 400.0;
    smoothed.handleCountAvailable = false;
    EXPECT_DOUBLE_EQ(countAxisDataMax(f.x, f.xMin, threads, handles, smoothed), 25.0);
    smoothed.handleCountAvailable = true;
    EXPECT_DOUBLE_EQ(countAxisDataMax(f.x, f.xMin, threads, handles, smoothed), 400.0);
}

TEST(ProcessDetailsChartHelpersTest, CountAxisWithGdiCoversAShorterGdiSeriesAndItsBar)
{
    const AxisFixture f;
    const std::array<double, 4> threads{0.0, 8.0, 9.0, 10.0};
    const std::array<double, 4> handles{0.0, 20.0, 21.0, 25.0};
    const std::array<double, 2> gdi{60.0, NOT_A_NUMBER}; // The last two samples only
    ProcessSmoothedUsage smoothed;
    EXPECT_DOUBLE_EQ(countAxisDataMaxWithGdi(f.x, f.xMin, threads, handles, gdi, smoothed), 60.0);
    smoothed.gdiObjectCount = 300.0;
    smoothed.gdiInitialized = false; // No reading: the bar shows N/A and does not move the axis
    EXPECT_DOUBLE_EQ(countAxisDataMaxWithGdi(f.x, f.xMin, threads, handles, gdi, smoothed), 60.0);
    smoothed.gdiInitialized = true;
    EXPECT_DOUBLE_EQ(countAxisDataMaxWithGdi(f.x, f.xMin, threads, handles, gdi, smoothed), 300.0);
    // With no GDI series at all it is countAxisDataMax().
    smoothed.gdiInitialized = false;
    EXPECT_DOUBLE_EQ(countAxisDataMaxWithGdi(f.x, f.xMin, threads, handles, std::span<const double>{}, smoothed),
                     countAxisDataMax(f.x, f.xMin, threads, handles, smoothed));
}

TEST(ProcessDetailsChartHelpersTest, RateAxesLeaveOutBarsWithoutAReading)
{
    const AxisFixture f;
    const std::array<double, 4> a{1.0e9, 100.0, NOT_A_NUMBER, 200.0};
    const std::array<double, 4> b{0.0, 300.0, 50.0, NOT_A_NUMBER};
    ProcessSmoothedUsage smoothed;
    smoothed.ioReadBytesPerSec = 5000.0;
    smoothed.ioWriteBytesPerSec = 6000.0;
    smoothed.netSentBytesPerSec = 7000.0;
    smoothed.netRecvBytesPerSec = 8000.0;
    smoothed.gpuMemoryBytes = 9000.0;

    // Unread: only the window's readings count.
    EXPECT_DOUBLE_EQ(ioAxisDataMax(f.x, f.xMin, a, b, smoothed), 300.0);
    EXPECT_DOUBLE_EQ(networkAxisDataMax(f.x, f.xMin, a, b, smoothed), 300.0);
    EXPECT_DOUBLE_EQ(gpuMemoryAxisDataMax(f.x, f.xMin, a, smoothed), 200.0);

    smoothed.ioAvailable = true;
    smoothed.networkAvailable = true;
    smoothed.gpuMemoryAvailable = true;
    EXPECT_DOUBLE_EQ(ioAxisDataMax(f.x, f.xMin, a, b, smoothed), 6000.0);
    EXPECT_DOUBLE_EQ(networkAxisDataMax(f.x, f.xMin, a, b, smoothed), 8000.0);
    EXPECT_DOUBLE_EQ(gpuMemoryAxisDataMax(f.x, f.xMin, a, smoothed), 9000.0);
}

TEST(ProcessDetailsChartHelpersTest, FaultAndPowerAxesCoverTheirSeriesAndBar)
{
    const AxisFixture f;
    const std::array<double, 4> series{50.0, 1.0, 2.0, NOT_A_NUMBER};
    ProcessSmoothedUsage smoothed;
    EXPECT_DOUBLE_EQ(faultAxisDataMax(f.x, f.xMin, series, smoothed), 2.0);
    EXPECT_DOUBLE_EQ(powerAxisDataMax(f.x, f.xMin, series, smoothed), 2.0);
    smoothed.pageFaultsPerSec = 3.0;
    smoothed.powerWatts = 4.0;
    EXPECT_DOUBLE_EQ(faultAxisDataMax(f.x, f.xMin, series, smoothed), 3.0);
    EXPECT_DOUBLE_EQ(powerAxisDataMax(f.x, f.xMin, series, smoothed), 4.0);
}

TEST(ProcessDetailsChartHelpersTest, AnEmptyWindowGivesZero)
{
    const std::span<const double> none;
    const ProcessSmoothedUsage smoothed;
    EXPECT_DOUBLE_EQ(cpuAxisDataMax(none, 0.0, none, none, none, smoothed), 0.0);
    EXPECT_DOUBLE_EQ(memoryAxisDataMax(none, 0.0, none, none, smoothed), 0.0);
    EXPECT_DOUBLE_EQ(countAxisDataMax(none, 0.0, none, none, smoothed), 0.0);
    EXPECT_DOUBLE_EQ(ioAxisDataMax(none, 0.0, none, none, smoothed), 0.0);
}

// ========== Empty states ==========

/// A history of @p count points, each built by @p make.
template<typename Make> [[nodiscard]] ProcessDetailsHistory historyOf(std::size_t count, const Make& make)
{
    ProcessDetailsHistory history;
    for (std::size_t i = 0; i < count; ++i)
    {
        history.append(static_cast<double>(i), make(i), false);
    }
    return history;
}

[[nodiscard]] ProcessHistoryPoint gapsOnly(std::size_t /*index*/)
{
    ProcessHistoryPoint point;
    point.ioRead = NOT_A_NUMBER;
    point.ioWrite = NOT_A_NUMBER;
    point.netSent = NOT_A_NUMBER;
    point.netReceived = NOT_A_NUMBER;
    point.gpuUtil = NOT_A_NUMBER;
    point.gpuMemory = NOT_A_NUMBER;
    return point;
}

TEST(ProcessDetailsChartHelpersTest, NetworkTabNeedsAReadingNotJustPoints)
{
    EXPECT_FALSE(hasNetworkTabData(ProcessDetailsHistory{}));
    EXPECT_FALSE(hasNetworkTabData(historyOf(5, gapsOnly)));
    // One reading of any of the four rates is data, a zero included.
    for (int which = 0; which < 4; ++which)
    {
        SCOPED_TRACE(which);
        const auto history =
            historyOf(5,
                      [which](std::size_t i)
                      {
                          ProcessHistoryPoint point = gapsOnly(i);
                          if (i == 3)
                          {
                              std::array<double*, 4> rates{&point.ioRead, &point.ioWrite, &point.netSent, &point.netReceived};
                              *rates.at(static_cast<std::size_t>(which)) = 0.0;
                          }
                          return point;
                      });
        EXPECT_TRUE(hasNetworkTabData(history));
    }
}

TEST(ProcessDetailsChartHelpersTest, GpuTabContentFollowsSupportReadingsAndUsage)
{
    const Domain::ProcessSnapshot idle;
    const ProcessDetailsHistory zeros = historyOf(5, [](std::size_t) { return ProcessHistoryPoint{}; });

    // No per-process support: unavailable, whatever the data says.
    Domain::ProcessSnapshot busy;
    busy.gpuUtilPercent = 30.0;
    EXPECT_EQ(gpuTabContentFor(busy, zeros, false), GpuTabContent::Unavailable);

    // Supported, every read failed: no readings yet.
    EXPECT_EQ(gpuTabContentFor(idle, historyOf(5, gapsOnly), true), GpuTabContent::NoReadings);
    EXPECT_EQ(gpuTabContentFor(idle, ProcessDetailsHistory{}, true), GpuTabContent::NoReadings);

    // Supported, read, all zero: no usage.
    EXPECT_EQ(gpuTabContentFor(idle, zeros, true), GpuTabContent::NoUsage);

    // Usage now, or anywhere in the retained history (#1014), or a device listed.
    EXPECT_EQ(gpuTabContentFor(busy, zeros, true), GpuTabContent::Usage);
    const auto usedEarlier = historyOf(5,
                                       [](std::size_t i)
                                       {
                                           ProcessHistoryPoint point;
                                           point.gpuMemory = (i == 1) ? 4096.0 : 0.0;
                                           return point;
                                       });
    EXPECT_EQ(gpuTabContentFor(idle, usedEarlier, true), GpuTabContent::Usage);
    Domain::ProcessSnapshot withDevice;
    withDevice.gpuDevices = "GPU0";
    EXPECT_EQ(gpuTabContentFor(withDevice, zeros, true), GpuTabContent::Usage);
}

} // namespace
} // namespace App::Detail
