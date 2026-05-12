#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/Linux/LinuxGPUProbe.h"

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

TEST(LinuxGPUProbeTest, EnumerateReadAndCapabilitiesDoNotThrow)
{
    LinuxGPUProbe probe;

    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto processCounters = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(LinuxGPUProbeTest, CounterGpuIdsAreNotEmptyWhenPresent)
{
    LinuxGPUProbe probe;
    const auto counters = probe.readGPUCounters();

    for (const auto& c : counters)
    {
        EXPECT_FALSE(c.gpuId.empty());
    }
}

TEST(LinuxGPUProbeTest, ProcessGpuCountersAreStructurallyValid)
{
    LinuxGPUProbe probe;
    const auto processCounters = probe.readProcessGPUCounters();

    for (const auto& c : processCounters)
    {
        EXPECT_GT(c.pid, 0);
        EXPECT_FALSE(c.gpuId.empty());
        EXPECT_GE(c.gpuUtilPercent, 0.0);
    }
}

} // namespace
} // namespace Platform

#endif
