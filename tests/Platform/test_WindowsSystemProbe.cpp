/// @file test_WindowsSystemProbe.cpp
/// @brief Integration tests for Platform::WindowsSystemProbe

#include "Platform/SystemTypes.h"
#include "Platform/Windows/WindowsSystemProbe.h"
#include "Platform/Windows/WindowsSystemProbeMath.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

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

// =============================================================================
// WindowsSystemProbeMath: pure helpers, fabricated inputs
// =============================================================================

namespace
{

/// One SYSTEM_PAGEFILE_INFORMATION-shaped entry: next offset, size, in use, peak, then 16 bytes of
/// (ignored) file-name UNICODE_STRING.
void appendPageFileEntry(std::vector<std::byte>& buffer, std::uint32_t next, std::uint32_t total, std::uint32_t inUse)
{
    const std::size_t at = buffer.size();
    buffer.resize(at + 32);
    const std::array<std::uint32_t, 4> header{next, total, inUse, inUse};
    std::memcpy(buffer.data() + at, header.data(), sizeof(header));
}

} // namespace

TEST(WindowsSystemProbeMathTest, SumsEveryPageFileInTheChain)
{
    std::vector<std::byte> buffer;
    appendPageFileEntry(buffer, 32, 1'048'576, 58'381); // 4 GiB file, ~228 MiB in use
    appendPageFileEntry(buffer, 0, 262'144, 1'000);     // 1 GiB file

    const auto result = sumPageFiles(buffer);
    ASSERT_TRUE(result.has_value());
    const PageFileTotals totals = result.value_or(PageFileTotals{});
    EXPECT_EQ(totals.totalPages, 1'310'720ULL);
    EXPECT_EQ(totals.inUsePages, 59'381ULL);

    const SwapBytes swap = swapFromPageFiles(totals, 4096);
    EXPECT_EQ(swap.totalBytes, 1'310'720ULL * 4096);
    EXPECT_EQ(swap.freeBytes, (1'310'720ULL - 59'381ULL) * 4096);
}

TEST(WindowsSystemProbeMathTest, MalformedChainsEndTheWalkWithoutReadingPastTheBuffer)
{
    // A link that points past the buffer is ignored after the entry it belongs to.
    std::vector<std::byte> pastEnd;
    appendPageFileEntry(pastEnd, 4096, 100, 10);
    const auto a = sumPageFiles(pastEnd);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a.value_or(PageFileTotals{}).totalPages, 100ULL);

    // A link shorter than an entry would loop or overlap; it ends the walk.
    std::vector<std::byte> tooShort;
    appendPageFileEntry(tooShort, 4, 100, 10);
    appendPageFileEntry(tooShort, 0, 999, 999);
    const auto b = sumPageFiles(tooShort);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b.value_or(PageFileTotals{}).totalPages, 100ULL);

    // Less than one entry is no answer at all.
    const std::vector<std::byte> truncated(8);
    EXPECT_FALSE(sumPageFiles(truncated).has_value());
}

TEST(WindowsSystemProbeMathTest, InUseAboveSizeIsCappedSoFreeCannotWrap)
{
    const SwapBytes swap = swapFromPageFiles({.totalPages = 100, .inUsePages = 150}, 4096);
    EXPECT_EQ(swap.totalBytes, 409'600ULL);
    EXPECT_EQ(swap.freeBytes, 0ULL);
}

TEST(WindowsSystemProbeMathTest, ProcessorTimesTakeInterruptAndDpcOutOfKernel)
{
    // kernel 1000 includes idle 600, so 400 busy, of which 50 interrupt and 30 DPC.
    const CpuCounters core = processorTimes(1000, 600, 200, 30, 50);
    EXPECT_EQ(core.idle, 600ULL);
    EXPECT_EQ(core.user, 200ULL);
    EXPECT_EQ(core.irq, 50ULL);
    EXPECT_EQ(core.softirq, 30ULL);
    EXPECT_EQ(core.system, 320ULL);
    // Each tick counted once: busy = user + kernel-busy, not + interrupt + DPC again.
    EXPECT_EQ(core.active(), 600ULL);
    EXPECT_EQ(core.total(), 1200ULL);
}

TEST(WindowsSystemProbeMathTest, ProcessorTimesClampComponentsThatOvershoot)
{
    // Counters are not sampled atomically, so a component can briefly exceed its parent.
    const CpuCounters idleAboveKernel = processorTimes(100, 150, 10, 5, 5);
    EXPECT_EQ(idleAboveKernel.system, 0ULL);
    EXPECT_EQ(idleAboveKernel.irq, 0ULL);
    EXPECT_EQ(idleAboveKernel.softirq, 0ULL);

    const CpuCounters dpcAboveBusy = processorTimes(1000, 900, 0, 500, 60);
    EXPECT_EQ(dpcAboveBusy.irq, 60ULL);
    EXPECT_EQ(dpcAboveBusy.softirq, 40ULL);
    EXPECT_EQ(dpcAboveBusy.system, 0ULL);
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
