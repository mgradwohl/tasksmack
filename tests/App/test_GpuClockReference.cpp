/// @file test_GpuClockReference.cpp
/// @brief Tests for GpuSection::gpuClockReferenceMHz(), the stable scale the GPU clock line and bar
/// are drawn against (#994).

#include "App/Panels/GpuSection.h"

#include <gtest/gtest.h>

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

} // namespace
} // namespace App
