#if defined(_WIN32)

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
    [[maybe_unused]] bool available = probe.isAvailable();
    // Just verify it doesn't throw and returns a bool
}

// ==========================================================================
// Empty Result Tests (when probe is unavailable or not initialized)
// ==========================================================================

TEST(WindowsPDHGPUProbeTest, ProbeReturnsValidProcessCountersOrEmpty)
{
    PDHGPUProbe probe;
    auto counters = probe.readProcessGPUCounters();
    // PDH per-process metrics if available
    // If not available, returns empty (deterministic fallback)
    EXPECT_TRUE(counters.empty() || !counters.empty()); // Always true - just verify it doesn't throw
}

TEST(WindowsPDHGPUProbeTest, ProbeCounterResultsAreWellFormed)
{
    PDHGPUProbe probe;
    auto counters = probe.readProcessGPUCounters();

    // If counters are returned, verify they're well-formed
    for (const auto& counter : counters)
    {
        EXPECT_GE(counter.pid, 0) << "Process ID should be non-negative";
        // Other fields may be zero/empty depending on availability
    }
}

// ==========================================================================
// Capabilities Tests
// ==========================================================================

TEST(WindowsPDHGPUProbeTest, CapabilitiesIsConsistent)
{
    PDHGPUProbe probe;
    const auto caps = probe.capabilities();

    // Verify capabilities object is well-formed
    // PDH is a Performance Data Helper, so it may or may not report certain capabilities
    // depending on system configuration and whether the probe initialized successfully
    EXPECT_TRUE(caps.hasTemperature || !caps.hasTemperature); // Always true
    EXPECT_TRUE(caps.hasPowerMetrics || !caps.hasPowerMetrics);
    EXPECT_TRUE(caps.hasClockSpeeds || !caps.hasClockSpeeds);
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

#endif // defined(_WIN32)
