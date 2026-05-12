#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/Linux/NVMLGPUProbe.h"

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

TEST(LinuxNVMLGPUProbeTest, BasicOperationsDoNotThrow)
{
    NVMLGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(LinuxNVMLGPUProbeTest, UnavailableProbeReportsNoCapabilities)
{
    NVMLGPUProbe probe;
    if (!probe.isAvailable())
    {
        const auto caps = probe.capabilities();
        EXPECT_FALSE(caps.hasTemperature);
        EXPECT_FALSE(caps.hasPowerMetrics);
        EXPECT_FALSE(caps.hasClockSpeeds);
        EXPECT_FALSE(caps.hasFanSpeed);
        EXPECT_FALSE(caps.hasPCIeMetrics);
        EXPECT_FALSE(caps.hasPerProcessMetrics);
        EXPECT_FALSE(caps.supportsMultiGPU);
        EXPECT_FALSE(caps.hasEngineUtilization);
    }
}

TEST(LinuxNVMLGPUProbeTest, AvailableProbeReturnsConsistentIds)
{
    NVMLGPUProbe probe;
    if (probe.isAvailable())
    {
        const auto gpus = probe.enumerateGPUs();
        const auto counters = probe.readGPUCounters();
        for (const auto& gpu : gpus)
        {
            EXPECT_FALSE(gpu.id.empty());
        }
        for (const auto& counter : counters)
        {
            EXPECT_FALSE(counter.gpuId.empty());
        }
    }
}

} // namespace
} // namespace Platform

#endif
