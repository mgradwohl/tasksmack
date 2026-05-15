#ifdef _WIN32

#include "Platform/Windows/NVMLGPUProbe.h"

#include <gtest/gtest.h>

#include <cstddef>

namespace Platform
{
namespace
{

// ==========================================================================
// Basic Smoke Tests
// ==========================================================================

TEST(WindowsNVMLGPUProbeTest, ConstructionDoesNotThrow)
{
    // Should not throw even if NVML is not available
    EXPECT_NO_THROW(NVMLGPUProbe probe);
}

TEST(WindowsNVMLGPUProbeTest, BasicOperationsDoNotThrow)
{
    NVMLGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

// ==========================================================================
// Availability Tests
// ==========================================================================

TEST(WindowsNVMLGPUProbeTest, IsAvailableReturnsBool)
{
    NVMLGPUProbe probe;
    const bool available1 = probe.isAvailable();
    const bool available2 = probe.isAvailable();
    EXPECT_EQ(available1, available2) << "isAvailable() should be stable across consecutive calls";
}

TEST(WindowsNVMLGPUProbeTest, AvailabilityMatchesCapabilities)
{
    NVMLGPUProbe probe;
    const bool available = probe.isAvailable();
    const auto caps = probe.capabilities();

    EXPECT_EQ(caps.hasTemperature, available);
    EXPECT_EQ(caps.hasPowerMetrics, available);
    EXPECT_EQ(caps.hasClockSpeeds, available);
    EXPECT_EQ(caps.hasFanSpeed, available);
    EXPECT_EQ(caps.hasPCIeMetrics, available);
    EXPECT_EQ(caps.supportsMultiGPU, available);
}

// ==========================================================================
// Empty Enumeration Tests (when probe is unavailable)
// ==========================================================================

TEST(WindowsNVMLGPUProbeTest, EnumerationBehaviorMatchesAvailability)
{
    NVMLGPUProbe probe;
    const bool available = probe.isAvailable();
    auto gpus = probe.enumerateGPUs();

    if (!available)
    {
        EXPECT_TRUE(gpus.empty()) << "Unavailable NVML should return empty GPU list";
    }
    else
    {
        for (const auto& gpu : gpus)
        {
            EXPECT_FALSE(gpu.id.empty()) << "GPU id should not be empty when NVML is available";
            EXPECT_FALSE(gpu.name.empty()) << "GPU name should not be empty when NVML is available";
        }
    }
}

TEST(WindowsNVMLGPUProbeTest, CounterDataIsWellFormedWhenPresent)
{
    NVMLGPUProbe probe;
    const bool available = probe.isAvailable();
    const auto gpus = probe.enumerateGPUs();
    auto counters = probe.readGPUCounters();

    if (available)
    {
        EXPECT_FALSE(gpus.empty()) << "Available NVML should enumerate at least one GPU before reading counters";
    }

    for (const auto& counter : counters)
    {
        EXPECT_FALSE(counter.gpuId.empty()) << "GPU counter id should not be empty";
        EXPECT_GE(counter.utilizationPercent, 0.0);
        EXPECT_LE(counter.utilizationPercent, 100.0);
    }
}

TEST(WindowsNVMLGPUProbeTest, ProcessCounterDataIsWellFormedWhenPresent)
{
    NVMLGPUProbe probe;
    const bool available = probe.isAvailable();
    const auto gpus = probe.enumerateGPUs();
    auto counters = probe.readProcessGPUCounters();

    if (available)
    {
        EXPECT_FALSE(gpus.empty()) << "Available NVML should enumerate at least one GPU before reading process counters";
    }

    for (const auto& counter : counters)
    {
        EXPECT_GE(counter.pid, 0) << "Process id should be non-negative";
    }
}

// ==========================================================================
// Capabilities Tests
// ==========================================================================

TEST(WindowsNVMLGPUProbeTest, CapabilitiesMatchAvailability)
{
    NVMLGPUProbe probe;
    const bool available = probe.isAvailable();
    const auto caps = probe.capabilities();

    if (!available)
    {
        EXPECT_FALSE(caps.hasTemperature);
        EXPECT_FALSE(caps.hasPowerMetrics);
        EXPECT_FALSE(caps.hasClockSpeeds);
        EXPECT_FALSE(caps.hasFanSpeed);
        EXPECT_FALSE(caps.hasPCIeMetrics);
        EXPECT_FALSE(caps.hasPerProcessMetrics);
        EXPECT_FALSE(caps.supportsMultiGPU);
    }
    else
    {
        EXPECT_TRUE(caps.hasTemperature);
        EXPECT_TRUE(caps.hasPowerMetrics);
        EXPECT_TRUE(caps.hasClockSpeeds);
        EXPECT_TRUE(caps.hasFanSpeed);
        EXPECT_TRUE(caps.hasPCIeMetrics);
        EXPECT_TRUE(caps.supportsMultiGPU);
    }
}

TEST(WindowsNVMLGPUProbeTest, AvailableProbeEnumerationIsStable)
{
    NVMLGPUProbe probe;
    if (!probe.isAvailable())
    {
        GTEST_SKIP() << "NVML not available (no NVIDIA GPU or driver detected)";
    }

    auto gpus1 = probe.enumerateGPUs();
    auto gpus2 = probe.enumerateGPUs();

    EXPECT_EQ(gpus1.size(), gpus2.size()) << "Available NVML enumeration should be stable across calls";

    for (std::size_t i = 0; i < gpus1.size(); ++i)
    {
        EXPECT_FALSE(gpus1[i].id.empty()) << "GPU id should not be empty when available";
        EXPECT_FALSE(gpus1[i].name.empty()) << "GPU name should not be empty when available";
        EXPECT_EQ(gpus1[i].id, gpus2[i].id) << "GPU id should be stable across enumerations";
        EXPECT_EQ(gpus1[i].name, gpus2[i].name) << "GPU name should be stable across enumerations";
    }
}

} // namespace
} // namespace Platform

#endif // _WIN32
