/// @file test_WindowsSystemProbe.cpp
/// @brief Integration tests for Platform::WindowsSystemProbe

#include "Platform/SystemTypes.h"
#include "Platform/Windows/WindowsSystemProbe.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace Platform
{
namespace
{
[[nodiscard]] uint64_t cpuComponentSum(const CpuCounters& c)
{
    return c.user + c.nice + c.system + c.idle + c.iowait + c.irq + c.softirq + c.steal + c.guest + c.guestNice;
}

} // namespace

TEST(WindowsSystemProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ WindowsSystemProbe probe; });
}

TEST(WindowsSystemProbeTest, CapabilitiesReportedCorrectly)
{
    WindowsSystemProbe probe;
    const auto caps = probe.capabilities();

    EXPECT_TRUE(caps.hasPerCoreCpu);
    EXPECT_TRUE(caps.hasMemoryAvailable);
    EXPECT_TRUE(caps.hasSwap);
    EXPECT_TRUE(caps.hasUptime);
    EXPECT_FALSE(caps.hasLoadAvg);
}

TEST(WindowsSystemProbeTest, TicksPerSecondMatchesFileTime)
{
    WindowsSystemProbe probe;
    EXPECT_EQ(probe.ticksPerSecond(), 10'000'000L);
}

TEST(WindowsSystemProbeTest, ReadReturnsValidCounters)
{
    WindowsSystemProbe probe;
    const auto counters = probe.read();

    EXPECT_GT(counters.cpuTotal.total(), 0ULL);
    EXPECT_EQ(cpuComponentSum(counters.cpuTotal), counters.cpuTotal.total());

    EXPECT_GT(counters.cpuPerCore.size(), 0ULL);

    EXPECT_GT(counters.memory.totalBytes, 0ULL);
    EXPECT_LE(counters.memory.availableBytes, counters.memory.totalBytes);
    EXPECT_LE(counters.memory.freeBytes, counters.memory.totalBytes);

    EXPECT_GT(counters.uptimeSeconds, 0ULL);

    EXPECT_GT(counters.hostname.size(), 0ULL);
    EXPECT_GT(counters.cpuModel.size(), 0ULL);
    EXPECT_GT(counters.cpuCoreCount, 0U);
}

TEST(WindowsSystemProbeTest, NetworkTotalIsTheSumOfTheReportedInterfaces)
{
    // NDIS filter rows repeat their adapter's counters; counting them made Total several times the
    // real traffic (#1030). With them skipped, Total is exactly the interfaces the probe reports,
    // and no two reported interfaces carry the same non-zero counters.
    WindowsSystemProbe probe;
    const auto counters = probe.read();

    std::uint64_t rx = 0;
    std::uint64_t tx = 0;
    for (std::size_t i = 0; i < counters.networkInterfaces.size(); ++i)
    {
        const auto& a = counters.networkInterfaces[i];
        rx += a.rxBytes;
        tx += a.txBytes;
        for (std::size_t j = i + 1; j < counters.networkInterfaces.size(); ++j)
        {
            const auto& b = counters.networkInterfaces[j];
            if (a.rxBytes != 0 || a.txBytes != 0)
            {
                EXPECT_FALSE(a.rxBytes == b.rxBytes && a.txBytes == b.txBytes)
                    << "'" << a.name << "' and '" << b.name << "' report identical counters (a filter row counted twice?)";
            }
        }
    }
    EXPECT_EQ(counters.netRxBytes, rx);
    EXPECT_EQ(counters.netTxBytes, tx);
}

TEST(WindowsSystemProbeTest, UptimeIncreases)
{
    WindowsSystemProbe probe;

    const auto counters1 = probe.read();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto counters2 = probe.read();

    EXPECT_GE(counters2.uptimeSeconds, counters1.uptimeSeconds);
}

} // namespace Platform
