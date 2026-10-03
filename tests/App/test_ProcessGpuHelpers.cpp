/// @file test_ProcessGpuHelpers.cpp
/// @brief Tests for App::Detail::hasGpuUsageToShow(), which decides whether the process details GPU
/// tab shows its charts or "No GPU usage detected" (#1014).

#include "App/Panels/ProcessDetailsPanel_GpuHelpers.h"

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

} // namespace
} // namespace App::Detail
