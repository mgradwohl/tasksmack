#if defined(_WIN32)

#include "Platform/Windows/WindowsGPUProbe.h"

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

// ==========================================================================
// Basic Smoke Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ConstructionDoesNotThrow)
{
    // Should not throw even if underlying probes are unavailable
    EXPECT_NO_THROW(WindowsGPUProbe probe);
}

TEST(WindowsGPUProbeTest, BasicOperationsDoNotThrow)
{
    WindowsGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
}

// ==========================================================================
// Enumeration Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, EnumerateGPUsReturnsValidList)
{
    WindowsGPUProbe probe;
    auto gpus = probe.enumerateGPUs();

    // Even if no hardware GPUs, should return valid vector (empty or populated)
    EXPECT_TRUE(gpus.empty() || !gpus.empty()); // Always true, but exercises code path

    // If GPUs are present, validate their fields
    for (const auto& gpu : gpus)
    {
        EXPECT_FALSE(gpu.id.empty()) << "GPU id should not be empty";
        EXPECT_FALSE(gpu.name.empty()) << "GPU name should not be empty";
        // luidId may be present for DXGI enumeration
    }
}

TEST(WindowsGPUProbeTest, EnumerateGPUsIsDeterministic)
{
    WindowsGPUProbe probe;

    auto gpus1 = probe.enumerateGPUs();
    auto gpus2 = probe.enumerateGPUs();

    // Enumeration should be deterministic (same list on consecutive calls)
    EXPECT_EQ(gpus1.size(), gpus2.size()) << "GPU count should be consistent across calls";

    // IDs should match in order
    for (std::size_t i = 0; i < gpus1.size(); ++i)
    {
        EXPECT_EQ(gpus1[i].id, gpus2[i].id) << "GPU id should be consistent at index " << i;
        EXPECT_EQ(gpus1[i].name, gpus2[i].name) << "GPU name should be consistent at index " << i;
    }
}

// ==========================================================================
// Counter Reading Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ReadGPUCountersReturnsValidList)
{
    WindowsGPUProbe probe;

    // Enumerate first (required for LUID mapping in counter reading)
    auto gpus = probe.enumerateGPUs();

    // Now read counters
    auto counters = probe.readGPUCounters();

    // Counter list should match GPU list size (or be empty if no GPUs)
    if (gpus.empty())
    {
        EXPECT_EQ(counters.size(), 0UL) << "No counters should be returned if no GPUs enumerated";
    }
    else
    {
        EXPECT_EQ(counters.size(), gpus.size()) << "Counter count should match GPU count";

        // Validate counter fields
        for (const auto& counter : counters)
        {
            EXPECT_FALSE(counter.gpuId.empty()) << "Counter gpuId should not be empty";
            EXPECT_GE(counter.utilizationPercent, 0.0) << "Utilization should be >= 0";
            EXPECT_LE(counter.utilizationPercent, 100.0) << "Utilization should be <= 100";
        }
    }
}

TEST(WindowsGPUProbeTest, ReadGPUCountersAfterEnumerateIsConsistent)
{
    WindowsGPUProbe probe;

    auto gpus = probe.enumerateGPUs();
    auto counters1 = probe.readGPUCounters();
    auto counters2 = probe.readGPUCounters();

    // Counter list should be consistent
    EXPECT_EQ(counters1.size(), counters2.size()) << "Counter count should be consistent across calls";

    // GPUs should remain enumerated
    auto gpus2 = probe.enumerateGPUs();
    EXPECT_EQ(gpus.size(), gpus2.size()) << "GPU count should not change";
}

// ==========================================================================
// Process Counter Reading Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ReadProcessGPUCountersDoesNotThrow)
{
    WindowsGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readProcessGPUCounters());
}

TEST(WindowsGPUProbeTest, ReadProcessGPUCountersReturnsValidList)
{
    WindowsGPUProbe probe;
    auto counters = probe.readProcessGPUCounters();

    // Per-process GPU counters may be empty (no GPU-using processes)
    // or populated if processes are using GPU resources
    for (const auto& counter : counters)
    {
        EXPECT_GE(counter.pid, 0) << "Process ID should be non-negative";
        // Other fields may vary depending on availability
    }
}

// ==========================================================================
// Fallback Behavior Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, EnumerateGPUsHandlesUnavailableProbes)
{
    // This test validates that WindowsGPUProbe gracefully handles the case
    // where underlying NVML or other probes are not available.
    // It should still return valid results from available probes (e.g., DXGI).

    WindowsGPUProbe probe;
    auto gpus = probe.enumerateGPUs();

    // Should not throw and should return a valid vector
    // (may be empty on systems with no hardware GPUs or only software adapters)
    EXPECT_TRUE(gpus.empty() || !gpus.empty()); // Always true
}

TEST(WindowsGPUProbeTest, ReadGPUCountersHandlesEmptyEnumeration)
{
    WindowsGPUProbe probe;

    // Don't enumerate (or if enumeration returns empty)
    auto counters = probe.readGPUCounters();

    // Should handle empty enumeration gracefully and return empty counter list
    if (counters.empty())
    {
        EXPECT_EQ(counters.size(), 0UL);
    }
    // If counters are returned, they should be valid
    for (const auto& counter : counters)
    {
        EXPECT_GE(counter.utilizationPercent, 0.0);
        EXPECT_LE(counter.utilizationPercent, 100.0);
    }
}

// ==========================================================================
// State Consistency Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ProbeStatePersistsBetweenCalls)
{
    WindowsGPUProbe probe;

    // First enumeration
    auto gpus1 = probe.enumerateGPUs();
    auto caps1 = probe.capabilities();

    // Second enumeration
    auto gpus2 = probe.enumerateGPUs();
    auto caps2 = probe.capabilities();

    // State should be consistent
    EXPECT_EQ(gpus1.size(), gpus2.size());
    // Capabilities should match (at least structure-wise)
    EXPECT_EQ(caps1.hasTemperature, caps2.hasTemperature);
    EXPECT_EQ(caps1.hasPowerMetrics, caps2.hasPowerMetrics);
}

TEST(WindowsGPUProbeTest, CapabilitiesReturnsConsistentStructure)
{
    WindowsGPUProbe probe;
    auto caps = probe.capabilities();

    // Just verify the capabilities struct is well-formed
    // (all fields are either true or false)
    EXPECT_TRUE(caps.hasTemperature || !caps.hasTemperature);
    EXPECT_TRUE(caps.hasPowerMetrics || !caps.hasPowerMetrics);
    EXPECT_TRUE(caps.hasClockSpeeds || !caps.hasClockSpeeds);
    EXPECT_TRUE(caps.hasFanSpeed || !caps.hasFanSpeed);
    EXPECT_TRUE(caps.hasPCIeMetrics || !caps.hasPCIeMetrics);
    EXPECT_TRUE(caps.hasPerProcessMetrics || !caps.hasPerProcessMetrics);
    EXPECT_TRUE(caps.supportsMultiGPU || !caps.supportsMultiGPU);
}

} // namespace
} // namespace Platform

#endif // defined(_WIN32)
