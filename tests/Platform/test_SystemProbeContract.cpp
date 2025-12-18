/// @file test_SystemProbeContract.cpp
/// @brief Cross-platform contract tests for Platform::ISystemProbe via Platform::makeSystemProbe()

#include "Platform/Factory.h"
#include "Platform/ISystemProbe.h"
#include "Platform/SystemTypes.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace Platform
{
namespace
{
[[nodiscard]] uint64_t cpuComponentSum(const CpuCounters& c)
{
    return c.user + c.nice + c.system + c.idle + c.iowait + c.irq + c.softirq + c.steal + c.guest + c.guestNice;
}

} // namespace

TEST(SystemProbeContractTest, FactoryConstructs)
{
    auto probe = makeSystemProbe();
    ASSERT_NE(probe, nullptr);
}

TEST(SystemProbeContractTest, TicksPerSecondIsPositive)
{
    auto probe = makeSystemProbe();
    ASSERT_NE(probe, nullptr);

    const long ticks = probe->ticksPerSecond();
    EXPECT_GT(ticks, 0);
    EXPECT_LE(ticks, 10'000'000L);
}

TEST(SystemProbeContractTest, ReadReturnsSaneCounters)
{
    auto probe = makeSystemProbe();
    ASSERT_NE(probe, nullptr);

    const auto caps = probe->capabilities();
    const SystemCounters counters = probe->read();

    EXPECT_GT(counters.cpuTotal.total(), 0ULL);
    EXPECT_EQ(cpuComponentSum(counters.cpuTotal), counters.cpuTotal.total());
    EXPECT_LE(counters.cpuTotal.active(), counters.cpuTotal.total());

    EXPECT_GT(counters.memory.totalBytes, 0ULL);
    EXPECT_LE(counters.memory.freeBytes, counters.memory.totalBytes);
    EXPECT_LE(counters.memory.availableBytes, counters.memory.totalBytes);

    if (caps.hasUptime)
    {
        EXPECT_GT(counters.uptimeSeconds, 0ULL);
    }

    EXPECT_GT(counters.hostname.size(), 0ULL);
    EXPECT_GT(counters.cpuCoreCount, 0);

    if (caps.hasPerCoreCpu)
    {
        EXPECT_GT(counters.cpuPerCore.size(), 0ULL);
    }
}

} // namespace Platform
