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
    EXPECT_EQ(classifyEmptyState(/*hasPublication=*/false, /*devicesKnown=*/false, /*deviceCount=*/0, /*snapshotCount=*/0),
              EmptyReason::Unavailable);
    EXPECT_EQ(classifyEmptyState(false, true, 2, 2), EmptyReason::Unavailable);
}

// Enumeration succeeded and found nothing: the one case that may say "no GPU detected".
TEST(GpuSectionEmptyStateTest, EnumeratedAndEmptyIsNoDevices)
{
    EXPECT_EQ(classifyEmptyState(true, true, 0, 0), EmptyReason::NoDevices);
}

// A reviewed defect: enumeration failed, so the empty device list means "could not look". Reporting
// that as "no GPU detected" would present a guess as a finding.
TEST(GpuSectionEmptyStateTest, FailedEnumerationIsUnavailableNotNoDevices)
{
    EXPECT_EQ(classifyEmptyState(true, false, 0, 0), EmptyReason::Unavailable);
}

// A reviewed defect: devices are known but the latest read returned nothing. That must not be
// reported as "no GPU detected" -- the GPU is there.
TEST(GpuSectionEmptyStateTest, KnownDevicesWithoutReadingsIsNoReadings)
{
    EXPECT_EQ(classifyEmptyState(true, true, 1, 0), EmptyReason::NoReadings);
    EXPECT_EQ(classifyEmptyState(true, true, 3, 0), EmptyReason::NoReadings);
}

TEST(GpuSectionEmptyStateTest, ReadingsMeanTheTabRenders)
{
    EXPECT_EQ(classifyEmptyState(true, true, 1, 1), EmptyReason::None);
    EXPECT_EQ(classifyEmptyState(true, true, 2, 1), EmptyReason::None);
    // Readings without a device list, or with enumeration having failed, still have something to
    // chart.
    EXPECT_EQ(classifyEmptyState(true, true, 0, 1), EmptyReason::None);
    EXPECT_EQ(classifyEmptyState(true, false, 0, 1), EmptyReason::None);
}

} // namespace
} // namespace App
