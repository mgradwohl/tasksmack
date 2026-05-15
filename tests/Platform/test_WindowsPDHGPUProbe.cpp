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
    EXPECT_NO_THROW(probe.setInstanceRefreshInterval(std::chrono::seconds(5)));
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
// Empty Result Tests (when probe is unavailable or not initialized)
// ==========================================================================

// ==========================================================================
// Capabilities Tests
// ==========================================================================

TEST(WindowsPDHGPUProbeTest, CapabilitiesRelationshipsAreConsistent)
{
    PDHGPUProbe probe;
    const auto caps = probe.capabilities();

    EXPECT_EQ(caps.hasPerProcessMetrics, caps.hasEngineUtilization);
    EXPECT_EQ(caps.hasPerProcessMetrics, caps.supportsMultiGPU);

    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasPowerMetrics);
    EXPECT_FALSE(caps.hasClockSpeeds);
    EXPECT_FALSE(caps.hasFanSpeed);
}

// ==========================================================================
// State Consistency Tests
// ==========================================================================

TEST(WindowsPDHGPUProbeTest, AvailabilityIsConsistentBetweenCalls)
{
    PDHGPUProbe probe;

    // Availability should not change within a single probe instance
    auto available1 = probe.isAvailable();
    auto available2 = probe.isAvailable();
    EXPECT_EQ(available1, available2) << "isAvailable() should return consistent value across calls";
}

TEST(WindowsPDHGPUProbeTest, SetInstanceRefreshIntervalDoesNotThrow)
{
    PDHGPUProbe probe;
    EXPECT_NO_THROW(probe.setInstanceRefreshInterval(std::chrono::seconds(5)));
    EXPECT_NO_THROW(probe.setInstanceRefreshInterval(std::chrono::seconds(10)));
    EXPECT_NO_THROW(probe.setInstanceRefreshInterval(std::chrono::seconds(1)));
}

} // namespace
} // namespace Platform

#endif // _WIN32
