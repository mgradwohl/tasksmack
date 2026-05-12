#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/Linux/ROCmGPUProbe.h"

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

TEST(LinuxROCmGPUProbeTest, BasicOperationsDoNotThrow)
{
    ROCmGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(LinuxROCmGPUProbeTest, UnavailableProbeReportsNoCapabilities)
{
    ROCmGPUProbe probe;
    if (!probe.isAvailable())
    {
        const auto caps = probe.capabilities();
        EXPECT_FALSE(caps.hasTemperature);
        EXPECT_FALSE(caps.hasHotspotTemp);
        EXPECT_FALSE(caps.hasPowerMetrics);
        EXPECT_FALSE(caps.hasClockSpeeds);
        EXPECT_FALSE(caps.hasFanSpeed);
        EXPECT_FALSE(caps.hasPerProcessMetrics);
    }
}

TEST(LinuxROCmGPUProbeTest, ProcessCountersAreEmptyWhenAvailableOrUnavailable)
{
    ROCmGPUProbe probe;
    const auto processCounters = probe.readProcessGPUCounters();
    EXPECT_TRUE(processCounters.empty());
}

} // namespace
} // namespace Platform

#endif
