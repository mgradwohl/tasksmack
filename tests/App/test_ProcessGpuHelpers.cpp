/// @file test_ProcessGpuHelpers.cpp
/// @brief Tests for App::Detail::hasGpuUsageToShow(), which decides whether the process details GPU
/// tab shows its charts or "No GPU usage detected" (#1014).

#include "App/Panels/ProcessDetailsPanel_GpuHelpers.h"
#include "UI/Format.h"

#include <gtest/gtest.h>

#include <deque>

namespace App::Detail
{
namespace
{

TEST(ProcessGpuHelpersTest, NoCurrentUseAndNoHistoryShowsTheMessage)
{
    const std::deque<double> empty;
    EXPECT_FALSE(hasGpuUsageToShow(0, 0.0, false, empty, empty));

    const std::deque<double> zeros{0.0, 0.0, 0.0};
    EXPECT_FALSE(hasGpuUsageToShow(0, 0.0, false, zeros, zeros));
}

TEST(ProcessGpuHelpersTest, AnyCurrentUseShowsTheCharts)
{
    const std::deque<double> empty;
    EXPECT_TRUE(hasGpuUsageToShow(4096, 0.0, false, empty, empty));
    EXPECT_TRUE(hasGpuUsageToShow(0, 3.5, false, empty, empty));
    EXPECT_TRUE(hasGpuUsageToShow(0, 0.0, true, empty, empty));
}

// The #1014 case: the process has just gone idle on the GPU, but its history still holds the use.
TEST(ProcessGpuHelpersTest, IdleNowWithUseInHistoryKeepsTheCharts)
{
    const std::deque<double> util{0.0, 12.0, 0.0};
    const std::deque<double> memory{0.0, 0.0, 0.0};
    EXPECT_TRUE(hasGpuUsageToShow(0, 0.0, false, util, memory));

    const std::deque<double> noUtil{0.0, 0.0};
    const std::deque<double> someMemory{0.0, 1048576.0};
    EXPECT_TRUE(hasGpuUsageToShow(0, 0.0, false, noUtil, someMemory));
}

// Once every sample with GPU use has aged out of the window, the message comes back.
TEST(ProcessGpuHelpersTest, HistoryThatHasAgedToAllZeroShowsTheMessage)
{
    std::deque<double> util{8.0, 0.0, 0.0};
    std::deque<double> memory{2048.0, 0.0, 0.0};
    EXPECT_TRUE(hasGpuUsageToShow(0, 0.0, false, util, memory));
    util.pop_front();
    memory.pop_front();
    EXPECT_FALSE(hasGpuUsageToShow(0, 0.0, false, util, memory));
}

// #1207: one GPU's breakdown repeated the usage table above it.
TEST(ProcessGpuHelpersTest, PerGpuBreakdownOnlyForMoreThanOneGpu)
{
    EXPECT_FALSE(shouldShowPerGpuBreakdown(0));
    EXPECT_FALSE(shouldShowPerGpuBreakdown(1));
    EXPECT_TRUE(shouldShowPerGpuBreakdown(2));
    EXPECT_TRUE(shouldShowPerGpuBreakdown(4));
}

// #1210: the "No GPU usage" text says which window was looked at, since a process that used the GPU
// before the retained history gets it too.
TEST(ProcessGpuHelpersTest, NoGpuUsageTextNamesTheHistoryWindow)
{
    EXPECT_EQ(noGpuUsageDetail(300.0), "This process has not used a GPU in its retained history (up to 5m).");
    EXPECT_EQ(noGpuUsageDetail(90.0), "This process has not used a GPU in its retained history (up to 1m 30s).");
    EXPECT_EQ(noGpuUsageDetail(3600.0), "This process has not used a GPU in its retained history (up to 1h).");
}

// #1210: NVML on Linux reports a process's GPU memory but not its utilization; the tab shows N/A,
// not a measured 0%.
TEST(ProcessGpuHelpersTest, GpuUtilizationIsNotAvailableWithoutPerProcessUtilization)
{
    EXPECT_EQ(gpuUtilizationText(/*perProcessUtilizationSupported=*/false, 0.0), "N/A");
    EXPECT_EQ(gpuUtilizationText(false, 42.0), "N/A");
    EXPECT_EQ(gpuUtilizationText(true, 42.0), UI::Format::percentOneDecimal(42.0));
    EXPECT_EQ(gpuUtilizationText(true, 0.0), UI::Format::percentOneDecimal(0.0)); // A measured zero
}

// #1210: without per-process GPU metrics (DRM- or ROCm-only Linux) the tab must not claim the process
// used no GPU, since no use could be seen.
TEST(ProcessGpuHelpersTest, GpuTabSaysUnavailableRatherThanNoUsageWithoutPerProcessMetrics)
{
    EXPECT_EQ(gpuTabContent(/*perProcessGpuSupported=*/false, /*hasUsageToShow=*/false), GpuTabContent::Unavailable);
    EXPECT_EQ(gpuTabContent(false, true), GpuTabContent::Unavailable);
    EXPECT_EQ(gpuTabContent(true, false), GpuTabContent::NoUsage);
    EXPECT_EQ(gpuTabContent(true, true), GpuTabContent::Usage);
}

// #1210: a supported probe whose every retained read failed has a history of only gaps. That is no
// reading yet, not a measured lack of GPU use, and must not say the process has not used a GPU.
TEST(ProcessGpuHelpersTest, OnlyFailedReadsIsNoReadingsNotNoUsage)
{
    EXPECT_EQ(gpuTabContent(/*perProcessGpuSupported=*/true, /*hasUsageToShow=*/false, /*hasAnyReading=*/false), GpuTabContent::NoReadings);
    EXPECT_EQ(gpuTabContent(true, false, /*hasAnyReading=*/true), GpuTabContent::NoUsage); // Measured zeros
    EXPECT_EQ(gpuTabContent(true, true, false), GpuTabContent::Usage);
    EXPECT_EQ(gpuTabContent(false, false, false), GpuTabContent::Unavailable); // Unsupported still wins
}

} // namespace
} // namespace App::Detail
