#ifdef _WIN32

#include "Platform/Windows/WindowsGPUProbe.h"

#include <gtest/gtest.h>

#include <unordered_set>

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

    std::unordered_set<std::string> ids;

    // If GPUs are present, validate their fields
    for (const auto& gpu : gpus)
    {
        EXPECT_FALSE(gpu.id.empty()) << "GPU id should not be empty";
        EXPECT_FALSE(gpu.name.empty()) << "GPU name should not be empty";
        EXPECT_TRUE(ids.insert(gpu.id).second) << "GPU ids should be unique";
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

    // Capability relationships are deterministic across DXGI + NVML + PDH composition.
    EXPECT_EQ(caps.hasPowerMetrics, caps.hasClockSpeeds);
    EXPECT_EQ(caps.hasFanSpeed, caps.hasTemperature);
    EXPECT_EQ(caps.hasPCIeMetrics, caps.hasTemperature);
    EXPECT_EQ(caps.hasPerProcessMetrics, caps.hasEngineUtilization);
}

} // namespace
} // namespace Platform

#endif // _WIN32
