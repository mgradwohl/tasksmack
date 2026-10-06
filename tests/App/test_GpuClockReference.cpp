/// @file test_GpuClockReference.cpp
/// @brief Tests for GpuSection::gpuClockReferenceMHz(), the stable scale the GPU clock line and bar
/// are drawn against (#994).

#include "App/Panels/GpuSection.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <vector>

namespace App
{
namespace
{

using GpuSection::GPU_CLOCK_REFERENCE_FLOOR_MHZ;
using GpuSection::gpuClockReferenceMHz;

TEST(GpuClockReferenceTest, IdleClocksUseTheFloor)
{
    const std::vector<float> history{300.0F, 450.0F, 600.0F};
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(history, 450), GPU_CLOCK_REFERENCE_FLOOR_MHZ);
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz({}, 0), GPU_CLOCK_REFERENCE_FLOOR_MHZ);
}

TEST(GpuClockReferenceTest, ReferenceIsTheHistoryPeakNotTheCurrentClock)
{
    // The old scale was max(current, 2000): the 2400 MHz sample would read 120 % against a current
    // 2000 MHz and be clipped, and the whole history rescaled whenever the current clock moved.
    const std::vector<float> history{2100.0F, 2400.0F, 2200.0F, 2000.0F};
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(history, 2000), 2400.0F);
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(history, 2200), 2400.0F);
}

TEST(GpuClockReferenceTest, ACurrentClockAboveTheHistoryRaisesTheReference)
{
    const std::vector<float> history{2100.0F, 2200.0F};
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(history, 2500), 2500.0F);
}

TEST(GpuClockReferenceTest, TheReferenceFallsOnceThePeakAgesOut)
{
    const std::vector<float> withPeak{2600.0F, 2100.0F, 2200.0F};
    const std::vector<float> peakTrimmed{2100.0F, 2200.0F};
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(withPeak, 2200), 2600.0F);
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(peakTrimmed, 2200), 2200.0F);
}

TEST(GpuClockReferenceTest, NonFiniteSamplesAreIgnored)
{
    const std::vector<float> history{
        std::numeric_limits<float>::quiet_NaN(), 2300.0F, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(history, 0), 2300.0F);
}

TEST(GpuClockReferenceTest, NoSampleInTheHistoryExceedsTheReference)
{
    const std::vector<float> history{1800.0F, 2750.0F, 2300.0F, 900.0F};
    const float reference = gpuClockReferenceMHz(history, 1200);
    for (const float clock : history)
    {
        EXPECT_LE(clock / reference, 1.0F);
    }
}

// The xMin overload (#1324): only the samples the window shows set the reference.

TEST(GpuClockReferenceTest, APeakBeforeTheWindowDoesNotSetTheReference)
{
    // A boost spike at x = -61 has just scrolled out of a 60 s window; trimming keeps it as the anchor
    // left of the edge. The idle clocks in the window must not be scaled against it.
    const std::vector<double> time{-61.0, -40.0, -20.0, 0.0};
    const std::vector<float> clocks{2800.0F, 2100.0F, 2300.0F, 2200.0F};
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(time, -60.0, clocks, 2200), 2300.0F);
    // The whole-history reference still sees it, which is what #1324 was.
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(clocks, 2200), 2800.0F);
}

TEST(GpuClockReferenceTest, APeakInsideTheWindowStillSetsTheReference)
{
    const std::vector<double> time{-61.0, -40.0, -20.0, 0.0};
    const std::vector<float> clocks{2100.0F, 2700.0F, 2300.0F, 2200.0F};
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(time, -60.0, clocks, 2200), 2700.0F);
}

TEST(GpuClockReferenceTest, ASampleExactlyAtXMinCounts)
{
    const std::vector<double> time{-60.0, -30.0, 0.0};
    const std::vector<float> clocks{2600.0F, 2100.0F, 2200.0F};
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(time, -60.0, clocks, 2200), 2600.0F);
}

TEST(GpuClockReferenceTest, WithNoSampleInTheWindowTheCurrentClockAndFloorDecide)
{
    // Scrolled back past every sample, or only the trim anchor left: the floor, or the current clock
    // above it.
    const std::vector<double> time{-120.0, -100.0};
    const std::vector<float> clocks{2900.0F, 2600.0F};
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(time, -60.0, clocks, 500), GPU_CLOCK_REFERENCE_FLOOR_MHZ);
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(time, -60.0, clocks, 2400), 2400.0F);
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz({}, -60.0, {}, 0), GPU_CLOCK_REFERENCE_FLOOR_MHZ);
}

TEST(GpuClockReferenceTest, TheWindowedReferenceIgnoresNonFiniteSamples)
{
    const std::vector<double> time{-30.0, -20.0, -10.0, 0.0};
    const std::vector<float> clocks{
        std::numeric_limits<float>::quiet_NaN(), 2300.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(time, -60.0, clocks, 0), 2300.0F);
}

TEST(GpuClockReferenceTest, ClocksAlignToTheTailOfTheTimeAxis)
{
    // The clock series may be shorter than the axis (tail-aligned, like tailAlignedSpan()): its last
    // value is at the axis's last time, so here 2900 is at x = -61, before the window.
    const std::vector<double> time{-90.0, -61.0, -30.0, 0.0};
    const std::vector<float> clocks{2900.0F, 2100.0F, 2250.0F};
    EXPECT_FLOAT_EQ(gpuClockReferenceMHz(time, -60.0, clocks, 0), 2250.0F);
}

TEST(GpuClockReferenceTest, NoVisibleSampleExceedsTheWindowedReference)
{
    const std::vector<double> time{-70.0, -45.0, -30.0, -15.0, 0.0};
    const std::vector<float> clocks{3100.0F, 1800.0F, 2750.0F, 2300.0F, 900.0F};
    const float reference = gpuClockReferenceMHz(time, -60.0, clocks, 1200);
    EXPECT_FLOAT_EQ(reference, 2750.0F);
    for (std::size_t i = 1; i < clocks.size(); ++i)
    {
        EXPECT_LE(clocks[i] / reference, 1.0F);
    }
}

} // namespace
} // namespace App
