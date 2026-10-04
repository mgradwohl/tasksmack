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
#include <optional>
#include <span>
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

TEST(WindowsSystemProbeTest, NetworkTotalIsTheSumOfTheReportedInterfaces)
{
    // Total is exactly the interfaces the probe reports (#1030). Which rows are reported -- filter
    // rows excluded -- is tested with controlled rows in isCountedNetworkRow's tests below.
    WindowsSystemProbe probe;
    const auto counters = probe.read();

    std::uint64_t rx = 0;
    std::uint64_t tx = 0;
    for (const auto& iface : counters.networkInterfaces)
    {
        rx += iface.rxBytes;
        tx += iface.txBytes;
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

namespace
{
// Stand-in for SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION: the core's identity is its user time.
struct FakeProcessorEntry
{
    std::uint64_t id = 0;
    std::uint64_t padding = 0;
};

[[nodiscard]] CpuCounters fakeEntryToCounters(const FakeProcessorEntry& entry)
{
    CpuCounters core{};
    core.user = entry.id;
    return core;
}
} // namespace

TEST(WindowsSystemProbeMathTest, ProcessorGroupsAreAppendedInOrderAndCountIsTheSum)
{
    // Two processor groups, as on a >64-logical-processor machine (#1107): group 0 has 4 cores,
    // group 1 has 3. Every core of both appears, group 0's first.
    const std::array<FakeProcessorEntry, 4> group0{{{.id = 0}, {.id = 1}, {.id = 2}, {.id = 3}}};
    const std::array<FakeProcessorEntry, 3> group1{{{.id = 100}, {.id = 101}, {.id = 102}}};

    std::vector<CpuCounters> cores;
    EXPECT_EQ(appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group0), sizeof(group0), fakeEntryToCounters), 4U);
    EXPECT_EQ(appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group1), sizeof(group1), fakeEntryToCounters), 3U);

    ASSERT_EQ(cores.size(), group0.size() + group1.size());
    const std::array<std::uint64_t, 7> expectedOrder{0, 1, 2, 3, 100, 101, 102};
    for (std::size_t i = 0; i < expectedOrder.size(); ++i)
    {
        EXPECT_EQ(cores[i].user, expectedOrder[i]) << "core " << i;
    }
}

TEST(WindowsSystemProbeMathTest, ShortProcessorGroupAppendsOnlyTheReturnedEntries)
{
    // The buffer had room for 4 entries but the group reported 2 (fewer processors than sized for).
    const std::array<FakeProcessorEntry, 4> group{{{.id = 7}, {.id = 8}, {.id = 9}, {.id = 10}}};
    std::vector<CpuCounters> cores{CpuCounters{}};

    EXPECT_EQ(appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group), 2 * sizeof(FakeProcessorEntry), fakeEntryToCounters),
              2U);
    ASSERT_EQ(cores.size(), 3U);
    EXPECT_EQ(cores[1].user, 7U);
    EXPECT_EQ(cores[2].user, 8U);
}

TEST(WindowsSystemProbeMathTest, ProcessorGroupReturnLengthIsClampedToTheBuffer)
{
    // A partial trailing entry is dropped, and a length past the buffer cannot read beyond it.
    const std::array<FakeProcessorEntry, 2> group{{{.id = 1}, {.id = 2}}};
    std::vector<CpuCounters> cores;

    EXPECT_EQ(appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group), sizeof(FakeProcessorEntry) + 1, fakeEntryToCounters),
              1U);
    EXPECT_EQ(appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group), 64 * sizeof(FakeProcessorEntry), fakeEntryToCounters),
              2U);
    EXPECT_EQ(appendProcessorGroup(cores, std::span<const FakeProcessorEntry>(group), 0, fakeEntryToCounters), 0U);
    EXPECT_EQ(cores.size(), 3U);
}

// #1107 review: on a machine with several processor groups, Total is the sum of every group's
// cores, so an idle group and a busy group of equal size read about 50 %, not one group's figure.
TEST(SumCpuCountersTest, TotalIsTheSumOfEveryCoreInEveryGroup)
{
    std::vector<CpuCounters> cores(4);
    cores[0].user = 100; // Group 0: busy
    cores[1].user = 100;
    cores[2].idle = 100; // Group 1: idle
    cores[3].idle = 100;
    cores[3].irq = 7;

    const CpuCounters total = sumCpuCounters(cores);
    EXPECT_EQ(total.user, 200U);
    EXPECT_EQ(total.idle, 200U);
    EXPECT_EQ(total.irq, 7U);
    EXPECT_EQ(total.total(), 407U);
}

TEST(SumCpuCountersTest, NoCoresIsAllZero)
{
    EXPECT_EQ(sumCpuCounters({}).total(), 0U);
}

// #1107 review: a multi-group machine never reports a one-group Total. Before the first complete
// all-group read it reports a zeroed one; the first complete read then measures from zero (the
// average since boot) instead of comparing the all-group sum with a one-group baseline, which put
// the other groups' lifetime counters into one interval as a spike.
TEST(MultiGroupTotalTest, FirstReadFailureThenRecoveryDoesNotSpike)
{
    std::optional<CpuCounters> last;
    const CpuCounters failed = multiGroupTotal(std::nullopt, last);
    EXPECT_EQ(failed.total(), 0U);
    EXPECT_FALSE(last.has_value());

    CpuCounters allGroups; // Lifetime counters of every group: 25 % busy since boot
    allGroups.user = 250;
    allGroups.idle = 750;
    const CpuCounters recovered = multiGroupTotal(allGroups, last);
    ASSERT_TRUE(last.has_value());

    // The model's busy share over the recovery interval: the since-boot average, not a spike.
    const double busy = static_cast<double>(recovered.active() - failed.active());
    const double span = static_cast<double>(recovered.total() - failed.total());
    EXPECT_DOUBLE_EQ(100.0 * busy / span, 25.0);
}

TEST(MultiGroupTotalTest, ALaterFailureRepeatsTheLastAllGroupTotal)
{
    std::optional<CpuCounters> last;
    CpuCounters allGroups;
    allGroups.user = 40;
    allGroups.idle = 60;
    (void) multiGroupTotal(allGroups, last);

    const CpuCounters failed = multiGroupTotal(std::nullopt, last);
    EXPECT_EQ(failed.user, 40U);
    EXPECT_EQ(failed.idle, 60U);
}

TEST(WindowsSystemProbeTest, PerCoreCountMatchesCoreCount)
{
    // Per-core entries and the reported core count must both cover every processor group (#1107).
    WindowsSystemProbe probe;
    const auto counters = probe.read();

    EXPECT_EQ(counters.cpuPerCore.size(), counters.cpuCoreCount);
}

TEST(WindowsSystemProbeMathTest, FilterRowsAreNotCountedWhateverTheirType)
{
    // Filter-module rows carry their adapter's type and counters; the flag alone excludes them.
    EXPECT_FALSE(isCountedNetworkRow(IF_TYPE_WIFI, true));
    EXPECT_FALSE(isCountedNetworkRow(IF_TYPE_ETHERNET, true));
    EXPECT_FALSE(isCountedNetworkRow(IF_TYPE_VIRTUAL, true));
}

TEST(WindowsSystemProbeMathTest, RealInterfacesCountEvenWithIdenticalCounters)
{
    // Two distinct non-filter adapters can legitimately report the same bytes (e.g. both received
    // the same broadcast and sent nothing); the decision depends only on type and the filter flag.
    for (const std::uint32_t type : {IF_TYPE_ETHERNET, IF_TYPE_WIFI, IF_TYPE_TUNNEL_LINK, IF_TYPE_PPP_LINK, IF_TYPE_VIRTUAL})
    {
        EXPECT_TRUE(isCountedNetworkRow(type, false)) << type;
    }
}

TEST(WindowsSystemProbeMathTest, LoopbackAndOtherTypesAreNotCounted)
{
    EXPECT_FALSE(isCountedNetworkRow(IF_TYPE_LOOPBACK, false));
    EXPECT_FALSE(isCountedNetworkRow(1, false)); // IF_TYPE_OTHER
    EXPECT_FALSE(isCountedNetworkRow(0, false));
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
