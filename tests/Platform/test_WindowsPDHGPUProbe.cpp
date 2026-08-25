#ifdef _WIN32

#include "Platform/Windows/PDHGPUProbe.h"

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

// ==========================================================================
// Basic Smoke Tests
// ==========================================================================

TEST(WindowsPDHGPUProbeTest, ConstructionDoesNotThrow)
{
    // Should not throw even if PDH is not available or initialization fails
    EXPECT_NO_THROW(PDHGPUProbe probe);
}

TEST(WindowsPDHGPUProbeTest, BasicOperationsDoNotThrow)
{
    PDHGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

// ==========================================================================
// Availability Tests
// ==========================================================================

TEST(WindowsPDHGPUProbeTest, IsAvailableReturnsBool)
{
    PDHGPUProbe probe;
    const bool available1 = probe.isAvailable();
    const bool available2 = probe.isAvailable();
    EXPECT_EQ(available1, available2) << "isAvailable() should be stable across consecutive calls";
}

// ==========================================================================
// Capabilities Tests
// ==========================================================================

TEST(WindowsPDHGPUProbeTest, CapabilitiesRelationshipsAreConsistent)
{
    PDHGPUProbe probe;
    const auto caps = probe.capabilities();

    EXPECT_EQ(caps.hasPerProcessMetrics, caps.hasEngineUtilization);

    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasPowerMetrics);
    EXPECT_FALSE(caps.hasClockSpeeds);
    EXPECT_FALSE(caps.hasFanSpeed);
}

// ==========================================================================
// State Consistency Tests
// ==========================================================================

} // namespace
} // namespace Platform

#endif // _WIN32
