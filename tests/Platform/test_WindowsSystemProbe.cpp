/// @file test_WindowsSystemProbe.cpp
/// @brief Integration tests for Platform::WindowsSystemProbe
///
/// The pure helpers (WindowsSystemProbeMath.h) are tested on every platform in
/// WindowsMath/test_WindowsSystemProbeMath.cpp.

#include "Platform/SystemTypes.h"
#include "Platform/Windows/WindowsSystemProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

namespace Platform
{
namespace
{
[[nodiscard]] uint64_t cpuComponentSum(const CpuCounters& c)
{
    // guest/guestNice are inside user/nice, so total() leaves them out (#1157).
    return c.user + c.nice + c.system + c.idle + c.iowait + c.irq + c.softirq + c.steal;
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

TEST(WindowsSystemProbeTest, NetworkTotalIsTheSumOfTheReportedHardwareInterfaces)
{
    // Total is the hardware interfaces the probe reports, or all of them if none is hardware
    // (#1030, #1257). Which rows are reported -- filter rows excluded -- and which count are tested
    // with controlled rows in isCountedNetworkRow's and sumCountedInterfaces' tests
    // (WindowsMath/test_WindowsSystemProbeMath.cpp).
    WindowsSystemProbe probe;
    const auto counters = probe.read();

    const bool anyHardware = std::ranges::any_of(counters.networkInterfaces, [](const auto& iface) { return !iface.isVirtual; });
    std::uint64_t rx = 0;
    std::uint64_t tx = 0;
    for (const auto& iface : counters.networkInterfaces)
    {
        if (!anyHardware || !iface.isVirtual)
        {
            rx += iface.rxBytes;
            tx += iface.txBytes;
        }
    }
    EXPECT_EQ(counters.netRxBytes, rx);
    EXPECT_EQ(counters.netTxBytes, tx);
}
TEST(WindowsSystemProbeTest, SwapIsThePageFileAndNeverExceedsIt)
{
    // Swap used to come from the commit figures and underflowed to ~100 % (#1026). Whatever this
    // machine's page file is, free can no longer exceed total.
    WindowsSystemProbe probe;
    const auto counters = probe.read();

    EXPECT_LE(counters.memory.swapFreeBytes, counters.memory.swapTotalBytes);
}

TEST(WindowsSystemProbeTest, CachedIsReportedAndWithinRam)
{
    // Cached was left at 0 on Windows (#1027). The system cache is never empty on a running system.
    WindowsSystemProbe probe;
    const auto counters = probe.read();

    EXPECT_GT(counters.memory.cachedBytes, 0ULL);
    EXPECT_LE(counters.memory.cachedBytes, counters.memory.totalBytes);
}

TEST(WindowsSystemProbeTest, PerCoreActiveTimeDoesNotExceedKernelPlusUser)
{
    // Interrupt and DPC time are inside kernel time; adding them again made every core read busier
    // than it was (#1032). Per core, active() must equal user + kernel-busy, so the sum over cores
    // of active() can never exceed the sum of total() (it used to, by the interrupt + DPC share).
    WindowsSystemProbe probe;
    const auto counters = probe.read();
    ASSERT_FALSE(counters.cpuPerCore.empty());
    for (const auto& core : counters.cpuPerCore)
    {
        EXPECT_LE(core.active(), core.total());
        EXPECT_EQ(core.active() + core.idle, core.total());
    }
}

TEST(WindowsSystemProbeTest, PerCoreCountMatchesCoreCount)
{
    // Per-core entries and the reported core count must both cover every processor group (#1107).
    WindowsSystemProbe probe;
    const auto counters = probe.read();

    EXPECT_EQ(counters.cpuPerCore.size(), counters.cpuCoreCount);
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
