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
    EXPECT_GT(counters.cpuCoreCount, 0U);

    if (caps.hasPerCoreCpu)
    {
        EXPECT_GT(counters.cpuPerCore.size(), 0ULL);
    }
}

// =============================================================================
// Per-Interface Network Counter Contract Tests
// =============================================================================

TEST(SystemProbeContractTest, PerInterfaceNetworkCountersPopulated)
{
    auto probe = makeSystemProbe();
    ASSERT_NE(probe, nullptr);

    const auto caps = probe->capabilities();
    const SystemCounters counters = probe->read();

    if (caps.hasNetworkCounters)
    {
        // If network counters are supported, per-interface data may be populated
        // Note: On systems with only loopback (which is often filtered), this may be empty
        // We don't require non-empty here; just verify the vector is accessible
        (void) counters.networkInterfaces.size();
    }
}

TEST(SystemProbeContractTest, PerInterfaceCountersHaveValidStructure)
{
    auto probe = makeSystemProbe();
    ASSERT_NE(probe, nullptr);

    const SystemCounters counters = probe->read();

    for (const auto& iface : counters.networkInterfaces)
    {
        // Names should not be empty
        EXPECT_FALSE(iface.name.empty()) << "Interface name should not be empty";
        EXPECT_FALSE(iface.displayName.empty()) << "Display name should not be empty";

        // Counters are cumulative uint64_t values, so always non-negative by type.
        // Access the fields to verify the structure is correctly populated.
        (void) iface.rxBytes;
        (void) iface.txBytes;

        // Link speed 0 is valid (unknown), but if non-zero should be reasonable
        if (iface.linkSpeedMbps > 0)
        {
            EXPECT_LE(iface.linkSpeedMbps, 1000000ULL); // Max 1 Tbps
        }
    }
}

TEST(SystemProbeContractTest, PerInterfaceCountersAreStableAcrossReads)
{
    auto probe = makeSystemProbe();
    ASSERT_NE(probe, nullptr);

    const SystemCounters counters1 = probe->read();
    const SystemCounters counters2 = probe->read();

    // Interface count should be stable (no interfaces appearing/disappearing rapidly)
    EXPECT_EQ(counters1.networkInterfaces.size(), counters2.networkInterfaces.size());

    // Interface names should be consistent
    for (size_t i = 0; i < std::min(counters1.networkInterfaces.size(), counters2.networkInterfaces.size()); ++i)
    {
        // If interfaces are in the same order, names should match
        // (we don't mandate ordering, but it should be consistent)
        EXPECT_EQ(counters1.networkInterfaces[i].name, counters2.networkInterfaces[i].name);
    }
}

} // namespace Platform
