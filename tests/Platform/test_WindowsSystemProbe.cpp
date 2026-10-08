/// @file test_WindowsSystemProbe.cpp
/// @brief Integration tests for Platform::WindowsSystemProbe
///
/// The pure helpers (WindowsSystemProbeMath.h) are tested on every platform in
/// WindowsMath/test_WindowsSystemProbeMath.cpp.

#include "Platform/CpuDetails.h"
#include "Platform/SystemTypes.h"
#include "Platform/Windows/ProcessorPerformanceCounter.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsSystemProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

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
    EXPECT_TRUE(caps.hasVirtualizationInfo); // #809
}

TEST(WindowsSystemProbeTest, CpuDetailsDescribeThisMachinesTopology)
{
    // GetLogicalProcessorInformationEx works for any user on any supported Windows (#809)
    WindowsSystemProbe probe;
    const auto counters = probe.read();
    const auto& cpu = counters.cpuDetails;
    ASSERT_TRUE(cpu.sockets.has_value());
    ASSERT_TRUE(cpu.physicalCores.has_value());
    ASSERT_TRUE(cpu.logicalProcessors.has_value());
    const std::size_t sockets = cpu.sockets.value_or(0);
    const std::size_t cores = cpu.physicalCores.value_or(0);
    const std::size_t logical = cpu.logicalProcessors.value_or(0);
    EXPECT_GE(sockets, 1U);
    EXPECT_GE(cores, sockets);
    EXPECT_GE(logical, cores);
    EXPECT_LE(logical, cores * 2); // At most two hardware threads a core
    // The same processors the per-core counters are sampled from (the active ones)
    EXPECT_EQ(logical, counters.cpuCoreCount);
    // A Hyper-V guest may report no cache records, so caches are checked only where present
    for (const auto& cache : {cpu.l1CacheBytes, cpu.l2CacheBytes, cpu.l3CacheBytes})
    {
        if (cache.has_value())
        {
            EXPECT_GT(cache.value_or(0), 0U);
        }
    }
    // A hybrid split, where there is one, accounts for every core
    EXPECT_EQ(cpu.performanceCores.has_value(), cpu.efficiencyCores.has_value());
    // Per-processor classes exactly where the CPU is hybrid, one per sampled processor
    EXPECT_EQ(cpu.performanceCores.has_value(), !cpu.efficiencyClassByCoreId.empty());
    EXPECT_EQ(std::ranges::count_if(cpu.efficiencyClassByCoreId, [](std::uint8_t c) { return c != UNKNOWN_EFFICIENCY_CLASS; }),
              cpu.efficiencyClassByCoreId.empty() ? 0 : static_cast<std::ptrdiff_t>(logical));
    if (cpu.performanceCores.has_value())
    {
        EXPECT_EQ(cpu.performanceCores.value_or(0) + cpu.efficiencyCores.value_or(0), cores);
    }
    // Virtualization facts are read without elevation; at least the processor-feature flags are known
    EXPECT_TRUE(cpu.virtualizationFirmwareEnabled.has_value());
    EXPECT_TRUE(cpu.slatSupported.has_value());
}

TEST(WindowsSystemProbeTest, CpuDetailsBaseSpeedIsNotTheRegistryClock)
{
    // The registry's ~MHz is not the nominal base clock on every CPU, so it is not published as one (#1530)
    WindowsSystemProbe probe(3686, nullptr);
    EXPECT_FALSE(probe.read().cpuDetails.baseSpeedMHz.has_value());
}

namespace
{
/// A fake pdh.dll for the current-clock tests (#1184): what each call returns, and what was called.
struct FakePdh
{
    double percent = 100.0;
    PDH_STATUS openStatus = ERROR_SUCCESS;
    PDH_STATUS addStatus = ERROR_SUCCESS;
    PDH_STATUS collectStatus = ERROR_SUCCESS;
    PDH_STATUS formatStatus = ERROR_SUCCESS;
    DWORD counterStatus = PDH_CSTATUS_VALID_DATA;
    bool addThrows = false;
    int closes = 0;
};
FakePdh g_FakePdh; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) - the fakes are plain function pointers

// Any non-null handle will do; the fakes never dereference it
const PDH_HQUERY FAKE_QUERY = &g_FakePdh;
const PDH_HCOUNTER FAKE_COUNTER = &g_FakePdh.percent;

PDH_STATUS WINAPI fakeOpenQuery(LPCWSTR /*source*/, DWORD_PTR /*userData*/, PDH_HQUERY* query)
{
    *query = FAKE_QUERY;
    return g_FakePdh.openStatus;
}

PDH_STATUS WINAPI fakeAddEnglishCounter(PDH_HQUERY /*query*/, LPCWSTR /*path*/, DWORD_PTR /*userData*/, PDH_HCOUNTER* counter)
{
    *counter = FAKE_COUNTER;
    if (g_FakePdh.addThrows)
    {
        throw std::runtime_error("fake PdhAddEnglishCounter threw");
    }
    return g_FakePdh.addStatus;
}

PDH_STATUS WINAPI fakeCollectQueryData(PDH_HQUERY /*query*/)
{
    return g_FakePdh.collectStatus;
}

PDH_STATUS WINAPI fakeGetFormattedCounterValue(PDH_HCOUNTER /*counter*/, DWORD format, LPDWORD /*type*/, PPDH_FMT_COUNTERVALUE value)
{
    value->CStatus = g_FakePdh.counterStatus;
    // As real PDH does, a percentage is capped at 100 unless PDH_FMT_NOCAP100 asks otherwise
    const double reading = ((format & PDH_FMT_NOCAP100) != 0) ? g_FakePdh.percent : std::min(g_FakePdh.percent, 100.0);
    value->doubleValue = reading; // NOLINT(cppcoreguidelines-pro-type-union-access) - PDH_FMT_DOUBLE's member
    return g_FakePdh.formatStatus;
}

PDH_STATUS WINAPI fakeCloseQuery(PDH_HQUERY /*query*/)
{
    ++g_FakePdh.closes;
    return ERROR_SUCCESS;
}

constexpr PdhFunctions FAKE_PDH_FUNCTIONS{
    .openQuery = &fakeOpenQuery,
    .addEnglishCounter = &fakeAddEnglishCounter,
    .collectQueryData = &fakeCollectQueryData,
    .getFormattedCounterValue = &fakeGetFormattedCounterValue,
    .closeQuery = &fakeCloseQuery,
};

constexpr std::uint64_t FAKE_BASE_MHZ = 3000;

/// Resets the fake pdh.dll before each test, so one test's failure mode doesn't leak into the next.
class WindowsSystemProbeCpuClockTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        g_FakePdh = FakePdh{};
    }
};
} // namespace

TEST_F(WindowsSystemProbeCpuClockTest, ClockFollowsProcessorPerformance)
{
    // The registry's ~MHz alone (the old figure) read 3000 for both of these.
    WindowsSystemProbe probe(FAKE_BASE_MHZ, ProcessorPerformanceCounter::open(FAKE_PDH_FUNCTIONS));
    g_FakePdh.percent = 75.0; // Power saving
    EXPECT_EQ(probe.read().cpuFreqMHz, 2250U);
    g_FakePdh.percent = 130.0; // Turbo
    EXPECT_EQ(probe.read().cpuFreqMHz, 3900U);
}

TEST_F(WindowsSystemProbeCpuClockTest, TurboReadsAboveTheBaseClock)
{
    // PDH caps a percentage at 100 unless asked not to; a capped turbo reading would read the base clock.
    WindowsSystemProbe probe(FAKE_BASE_MHZ, ProcessorPerformanceCounter::open(FAKE_PDH_FUNCTIONS));
    g_FakePdh.percent = 150.0;
    EXPECT_EQ(probe.read().cpuFreqMHz, 4500U);
}

namespace
{
/// A system DLL this process has not loaded, loaded now so its release can be observed (it is unloaded
/// again exactly when its only reference is freed); empty if every candidate is already loaded. The
/// fakes never call into it: it only stands in for pdh.dll's module, which the test process may hold.
[[nodiscard]] std::pair<const wchar_t*, Windows::UniqueModule> loadUnusedSystemDll()
{
    for (const wchar_t* name : {L"wtsapi32.dll", L"msimg32.dll", L"dciman32.dll", L"mprapi.dll", L"pdh.dll"})
    {
        if (GetModuleHandleW(name) == nullptr)
        {
            Windows::UniqueModule module(LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
            if (module != nullptr)
            {
                return {name, std::move(module)};
            }
        }
    }
    return {nullptr, Windows::UniqueModule{}};
}
} // namespace

TEST_F(WindowsSystemProbeCpuClockTest, ThrowWhileOpeningClosesTheQueryAndFreesTheModule)
{
    // Every resource open() acquired is released when a call after it throws: the opened query and the
    // module the functions came from.
    auto [name, module] = loadUnusedSystemDll();
    if (module == nullptr)
    {
        GTEST_SKIP() << "every candidate DLL is already loaded, so a release can't be observed";
    }
    g_FakePdh.addThrows = true;
    EXPECT_THROW((void) ProcessorPerformanceCounter::open(std::move(module), FAKE_PDH_FUNCTIONS), std::runtime_error);
    EXPECT_EQ(g_FakePdh.closes, 1) << "the opened query is closed";
    EXPECT_EQ(GetModuleHandleW(name), nullptr) << "the module is freed";
}

TEST_F(WindowsSystemProbeCpuClockTest, FailedOpenFreesTheModule)
{
    auto [name, module] = loadUnusedSystemDll();
    if (module == nullptr)
    {
        GTEST_SKIP() << "every candidate DLL is already loaded, so a release can't be observed";
    }
    g_FakePdh.openStatus = static_cast<PDH_STATUS>(PDH_INVALID_ARGUMENT);
    EXPECT_EQ(ProcessorPerformanceCounter::open(std::move(module), FAKE_PDH_FUNCTIONS), nullptr);
    EXPECT_EQ(GetModuleHandleW(name), nullptr) << "the module is freed";
}

TEST_F(WindowsSystemProbeCpuClockTest, NoCounterGivesTheBaseClock)
{
    WindowsSystemProbe probe(FAKE_BASE_MHZ, nullptr);
    EXPECT_EQ(probe.read().cpuFreqMHz, FAKE_BASE_MHZ);
}

TEST_F(WindowsSystemProbeCpuClockTest, CounterThatCannotBeOpenedGivesTheBaseClock)
{
    g_FakePdh.openStatus = static_cast<PDH_STATUS>(PDH_INVALID_ARGUMENT);
    EXPECT_EQ(ProcessorPerformanceCounter::open(FAKE_PDH_FUNCTIONS), nullptr);

    g_FakePdh = FakePdh{};
    g_FakePdh.addStatus = static_cast<PDH_STATUS>(PDH_CSTATUS_NO_COUNTER);
    auto counter = ProcessorPerformanceCounter::open(FAKE_PDH_FUNCTIONS);
    EXPECT_EQ(counter, nullptr);
    EXPECT_EQ(g_FakePdh.closes, 1) << "the opened query is closed when its counter can't be added";

    PdhFunctions missingExport = FAKE_PDH_FUNCTIONS;
    missingExport.getFormattedCounterValue = nullptr;
    EXPECT_EQ(ProcessorPerformanceCounter::open(missingExport), nullptr);

    WindowsSystemProbe probe(FAKE_BASE_MHZ, std::move(counter));
    EXPECT_EQ(probe.read().cpuFreqMHz, FAKE_BASE_MHZ);
}

TEST_F(WindowsSystemProbeCpuClockTest, FailedReadingGivesTheBaseClock)
{
    WindowsSystemProbe probe(FAKE_BASE_MHZ, ProcessorPerformanceCounter::open(FAKE_PDH_FUNCTIONS));
    g_FakePdh.percent = 75.0;

    g_FakePdh.collectStatus = static_cast<PDH_STATUS>(PDH_NO_DATA);
    EXPECT_EQ(probe.read().cpuFreqMHz, FAKE_BASE_MHZ);

    g_FakePdh.collectStatus = ERROR_SUCCESS;
    g_FakePdh.formatStatus = static_cast<PDH_STATUS>(PDH_INVALID_DATA);
    EXPECT_EQ(probe.read().cpuFreqMHz, FAKE_BASE_MHZ);

    g_FakePdh.formatStatus = ERROR_SUCCESS;
    g_FakePdh.counterStatus = PDH_CSTATUS_INVALID_DATA;
    EXPECT_EQ(probe.read().cpuFreqMHz, FAKE_BASE_MHZ);

    g_FakePdh.counterStatus = PDH_CSTATUS_VALID_DATA;
    EXPECT_EQ(probe.read().cpuFreqMHz, 2250U) << "a good reading after failures is used again";
}

TEST_F(WindowsSystemProbeCpuClockTest, QueryIsClosedWithTheProbe)
{
    {
        const WindowsSystemProbe probe(FAKE_BASE_MHZ, ProcessorPerformanceCounter::open(FAKE_PDH_FUNCTIONS));
    }
    EXPECT_EQ(g_FakePdh.closes, 1);
}

TEST(WindowsSystemProbeTest, CpuClockIsTheBaseClockScaledByProcessorPerformance)
{
    // The real pdh.dll on this machine (#1184): the base ~MHz scaled by "% Processor Performance" stays
    // within the range turbo and power saving can take it, and never reads 0 once the base is known.
    // The fakes above prove the reading is used; this checks the real counter path end to end.
    DWORD baseMHz = 0;
    DWORD dataSize = sizeof(baseMHz);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)",
                     L"~MHz",
                     RRF_RT_REG_DWORD,
                     nullptr,
                     &baseMHz,
                     &dataSize) != ERROR_SUCCESS ||
        baseMHz == 0)
    {
        GTEST_SKIP() << "no ~MHz base clock in the registry";
    }

    WindowsSystemProbe probe;
    EXPECT_TRUE(probe.capabilities().hasCpuFreq);
    (void) probe.read();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto counters = probe.read();
    EXPECT_GT(counters.cpuFreqMHz, 0U);
    EXPECT_LE(counters.cpuFreqMHz, std::uint64_t{baseMHz} * 10U);
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
