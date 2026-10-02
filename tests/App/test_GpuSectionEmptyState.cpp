/// @file test_GpuSectionEmptyState.cpp
/// @brief Tests for GpuSection::classifyEmptyState(), which decides what the GPU tab says when it
/// has nothing to chart (#927).

#include "App/Panels/GpuSection.h"

#include <gtest/gtest.h>

namespace App
{
namespace
{

using GpuSection::classifyEmptyState;
using GpuSection::EmptyReason;

// GPUModel publishes on every successful refresh, even one that finds no GPU, so the absence of a
// publication is a missing probe or a failed read -- never an ordinary wait.
TEST(GpuSectionEmptyStateTest, NoPublicationIsUnavailable)
{
    EXPECT_EQ(classifyEmptyState(/*hasPublication=*/false, /*deviceCount=*/0, /*snapshotCount=*/0), EmptyReason::Unavailable);
    EXPECT_EQ(classifyEmptyState(false, 2, 2), EmptyReason::Unavailable);
}

TEST(GpuSectionEmptyStateTest, NoDevicesAndNoReadingsIsNoDevices)
{
    EXPECT_EQ(classifyEmptyState(true, 0, 0), EmptyReason::NoDevices);
}

// The reviewed defect: devices are known but the latest read returned nothing. That must not be
// reported as "no GPU detected" -- the GPU is there.
TEST(GpuSectionEmptyStateTest, KnownDevicesWithoutReadingsIsNoReadings)
{
    EXPECT_EQ(classifyEmptyState(true, 1, 0), EmptyReason::NoReadings);
    EXPECT_EQ(classifyEmptyState(true, 3, 0), EmptyReason::NoReadings);
}

TEST(GpuSectionEmptyStateTest, ReadingsMeanTheTabRenders)
{
    EXPECT_EQ(classifyEmptyState(true, 1, 1), EmptyReason::None);
    EXPECT_EQ(classifyEmptyState(true, 2, 1), EmptyReason::None);
    // Readings without a matching device list still have something to chart.
    EXPECT_EQ(classifyEmptyState(true, 0, 1), EmptyReason::None);
}

} // namespace
} // namespace App
