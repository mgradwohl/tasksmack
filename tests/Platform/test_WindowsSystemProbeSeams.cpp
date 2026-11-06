/// @file test_WindowsSystemProbeSeams.cpp
/// @brief WindowsSystemProbe against fake Win32/NT calls (#1719): each CPU, memory, page-file and
/// topology query failing on its own, the per-core fallbacks, several processor groups and a
/// processor going on- or offline, none of which the real APIs can be made to show; and read()'s
/// sub-read timing (#2731) on a fake clock the fakes advance. The fakes are side-effect free; the
/// probe's own hostname, registry, power and network reads stay real (the network is faked in
/// test_WindowsSystemProbeNetwork.cpp).

#include "Domain/SamplingConfig.h"
#include "Platform/CpuDetails.h"
#include "Platform/SystemTypes.h"
#include "Platform/Windows/ProcessorPerformanceCounter.h" // IWYU pragma: keep (destroys the null counter)
#include "Platform/Windows/WindowsCommitPaging.h"
#include "Platform/Windows/WindowsSystemProbe.h"
#include "Platform/Windows/WindowsSystemProbeMath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
// clang-format on

// NOLINTBEGIN(misc-include-cleaner,cppcoreguidelines-pro-type-union-access,readability-non-const-parameter)
// Win32 types; the fakes fill OS-defined layouts (unions) and keep the exact Win32 signatures.
namespace Platform
{
namespace
{

constexpr LONG STATUS_ACCESS_DENIED_NT = static_cast<LONG>(0xC0000022L);
constexpr LONG STATUS_INFO_LENGTH_MISMATCH_NT = static_cast<LONG>(0xC0000004L);
constexpr LONG STATUS_BUFFER_TOO_SMALL_NT = static_cast<LONG>(0xC0000023L);
constexpr ULONG PROCESSOR_PERFORMANCE_CLASS = 8;
constexpr ULONG PAGE_FILE_CLASS = 18;
constexpr ULONG HYPERVISOR_CLASS = 91; // ARM64's hypervisor read (#2377); x64 asks CPUID instead
constexpr ULONG CODE_INTEGRITY_CLASS = 103;
constexpr ULONG ISOLATED_USER_MODE_CLASS = 165;
constexpr DWORD PAGE_SIZE = 4096;

/// SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION as the probe reads it.
struct PerfEntry
{
    LARGE_INTEGER idle;
    LARGE_INTEGER kernel;
    LARGE_INTEGER user;
    LARGE_INTEGER dpc;
    LARGE_INTEGER interrupt;
    ULONG interruptCount;
};

/// What every fake returns. Each processor group holds `activePerGroup[g]` processors, numbered
/// from that group's first id (the sum of the earlier groups' maxima).
struct FakeSystem // NOLINT(clang-analyzer-optin.performance.Padding)
{
    // GetSystemTimes, in 100 ns ticks
    bool failSystemTimes = false;
    std::uint64_t idle = 1'000;
    std::uint64_t kernel = 3'000; // Includes idle
    std::uint64_t user = 500;
    // Processor groups
    std::vector<DWORD> maxPerGroup{4};
    std::vector<DWORD> activePerGroup{4};
    WORD activeGroups = 1;
    // Per-core queries
    bool exAvailable = true;
    bool ntAvailable = true;
    int failExGroup = -1; // The group whose NtQuerySystemInformationEx fails; -1 = none
    bool failNtPerCore = false;
    DWORD extraCoresOnQuery = 0;    // Processors that appear between sizing and the query
    LONGLONG negativeIdleCore = -1; // A core whose IdleTime reads negative; -1 = none
    bool failAllGroupsCount = false;
    int exCalls = 0;
    int ntPerCoreCalls = 0;
    // Topology: the logical processors (group 0) it describes, one per core
    bool failTopologySizing = false;
    bool failTopologyRead = false;
    std::vector<std::size_t> topologyIds{0, 1, 2, 3};
    int topologyReads = 0;
    // Virtualization
    bool failCodeIntegrity = false;
    bool failIsolatedUserMode = false;
    ULONG codeIntegrityOptions = CODE_INTEGRITY_OPTION_HVCI_KMCI_ENABLED;
    // Where the last SystemHypervisorInformation and SystemIsolatedUserModeInformation queries were
    // told to write; 0 = not queried. Each structure holds a ULONGLONG the kernel checks alignment for (#2746).
    std::uintptr_t hypervisorBuffer = 0;
    std::uintptr_t isolatedUserModeBuffer = 0;
    // Memory
    bool failMemoryStatus = false;
    bool failPerformanceInfo = false;
    bool failPageFile = false;
    bool pageFileMismatchFirst = false;
    bool noPageFile = false;
    // The process list (#3363): whether it answers, the Memory Compression process's working set in it
    // (0 = no such process: compression is off), whether it first says the probe's buffer is too small,
    // and how many times it was asked
    bool processListAnswers = true;
    std::uint64_t compressionWorkingSet = 0;
    bool processListMismatchFirst = false;
    int processListQueries = 0;
    // The Memory Compression process's id and session (#3483), and one session's list
    // (SystemSessionProcessInformation): whether it answers, how many times it was asked and for which
    // session last
    std::uint64_t compressionProcessId = 3808;
    ULONG compressionSessionId = 0;
    bool sessionListAnswers = true;
    bool sessionListTooSmallAnswersBufferTooSmall = false;
    int sessionListQueries = 0;
    ULONG lastSessionQueried = 0xFFFFFFFF;
    // How far the fakes below advance the probe's clock (#2731), in place of the real calls' time
    std::chrono::milliseconds systemTimesTake{0};
    std::chrono::milliseconds memoryStatusTake{0};
};

FakeSystem& fake()
{
    static FakeSystem state;
    return state;
}

/// The probe's clock (#2731). Apart from FakeSystem so reading it cannot throw.
std::chrono::steady_clock::time_point& fakeClock() noexcept
{
    static std::chrono::steady_clock::time_point now;
    return now;
}

std::chrono::steady_clock::time_point fakeNow() noexcept
{
    return fakeClock();
}

void setFileTime(FILETIME& ft, std::uint64_t ticks)
{
    ULARGE_INTEGER value{};
    value.QuadPart = ticks;
    ft.dwLowDateTime = value.LowPart;
    ft.dwHighDateTime = value.HighPart;
}

BOOL WINAPI fakeGetSystemTimes(PFILETIME idle, PFILETIME kernel, PFILETIME user)
{
    fakeClock() += fake().systemTimesTake;
    if (fake().failSystemTimes)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    setFileTime(*idle, fake().idle);
    setFileTime(*kernel, fake().kernel);
    setFileTime(*user, fake().user);
    return TRUE;
}

WORD WINAPI fakeGetActiveProcessorGroupCount()
{
    return fake().activeGroups;
}

DWORD allActiveProcessors()
{
    DWORD total = 0;
    for (const DWORD count : fake().activePerGroup)
    {
        total += count;
    }
    return total;
}

DWORD WINAPI fakeGetActiveProcessorCount(WORD group)
{
    const auto& active = fake().activePerGroup;
    if (group == ALL_PROCESSOR_GROUPS)
    {
        return fake().failAllGroupsCount ? 0 : allActiveProcessors();
    }
    return group < active.size() ? active[group] : 0;
}

WORD WINAPI fakeGetMaximumProcessorGroupCount()
{
    return static_cast<WORD>(fake().maxPerGroup.size());
}

DWORD WINAPI fakeGetMaximumProcessorCount(WORD group)
{
    return group < fake().maxPerGroup.size() ? fake().maxPerGroup[group] : 0;
}

void WINAPI fakeGetSystemInfo(LPSYSTEM_INFO info)
{
    *info = SYSTEM_INFO{};
    info->dwPageSize = PAGE_SIZE;
    info->dwNumberOfProcessors = allActiveProcessors();
}

BOOL WINAPI fakeGetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP /*relation*/,
                                                 PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer,
                                                 PDWORD length)
{
    auto& f = fake();
    // One package, one core per described processor, one 1 MiB L2 cache
    const std::size_t records = f.topologyIds.size() + 2;
    const auto needed = static_cast<DWORD>(records * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX));
    if (buffer == nullptr)
    {
        if (f.failTopologySizing)
        {
            SetLastError(ERROR_INVALID_FUNCTION);
            return FALSE;
        }
        *length = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    ++f.topologyReads;
    if (f.failTopologyRead || *length < needed)
    {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX> out(records);
    for (auto& record : out)
    {
        record.Size = sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
    }
    out[0].Relationship = RelationProcessorPackage;
    std::size_t index = 1;
    for (const std::size_t id : f.topologyIds)
    {
        out[index].Relationship = RelationProcessorCore;
        out[index].Processor.GroupCount = 1;
        out[index].Processor.GroupMask[0].Group = 0;
        out[index].Processor.GroupMask[0].Mask = KAFFINITY{1} << id;
        ++index;
    }
    out[index].Relationship = RelationCache;
    out[index].Cache.Level = 2;
    out[index].Cache.CacheSize = 1U << 20U;
    std::memcpy(buffer, out.data(), needed);
    *length = needed;
    return TRUE;
}

/// Fill `buffer` with `cores` processors' times; a length mismatch when it is too small.
LONG fillPerformance(PVOID buffer, ULONG length, PULONG returned, DWORD cores)
{
    const auto needed = static_cast<ULONG>(cores * sizeof(PerfEntry));
    if (length < needed)
    {
        *returned = needed;
        return STATUS_INFO_LENGTH_MISMATCH_NT;
    }
    std::vector<PerfEntry> entries(cores);
    for (DWORD i = 0; i < cores; ++i)
    {
        const LONGLONG idle = (static_cast<LONGLONG>(i) + 1) * 100;
        entries[i].idle.QuadPart = std::cmp_equal(i, fake().negativeIdleCore) ? -5 : idle;
        entries[i].kernel.QuadPart = idle + 50;
        entries[i].user.QuadPart = 20;
    }
    std::memcpy(buffer, entries.data(), needed);
    *returned = needed;
    return 0;
}

LONG virtualizationQuery(ULONG infoClass, PVOID buffer)
{
    if (infoClass == CODE_INTEGRITY_CLASS)
    {
        if (fake().failCodeIntegrity)
        {
            return STATUS_ACCESS_DENIED_NT;
        }
        const std::array<ULONG, 2> info{2 * sizeof(ULONG), fake().codeIntegrityOptions}; // Length, CodeIntegrityOptions
        std::memcpy(buffer, info.data(), sizeof(info));
        return 0;
    }
    if (fake().failIsolatedUserMode)
    {
        return STATUS_ACCESS_DENIED_NT;
    }
    static_cast<std::uint8_t*>(buffer)[0] = 0x01; // The secure kernel is running
    return 0;
}

LONG pageFileQuery(PVOID buffer, ULONG length, PULONG returned)
{
    const auto& f = fake();
    if (f.failPageFile)
    {
        return STATUS_ACCESS_DENIED_NT;
    }
    if (f.noPageFile)
    {
        *returned = 0;
        return 0;
    }
    // Two entries, padded past the probe's first 4 KiB guess when asked for a mismatch
    const ULONG needed = f.pageFileMismatchFirst ? 6000 : 64;
    if (length < needed)
    {
        *returned = needed;
        return STATUS_INFO_LENGTH_MISMATCH_NT;
    }
    std::vector<std::byte> out(needed);
    const auto put = [&out](std::size_t offset, std::uint32_t value)
    {
        std::memcpy(&out[offset], &value, sizeof(value));
    };
    put(PAGEFILE_ENTRY_NEXT_OFFSET, 32);
    put(PAGEFILE_ENTRY_TOTAL_SIZE, 1000);
    put(PAGEFILE_ENTRY_TOTAL_IN_USE, 250);
    put(32 + PAGEFILE_ENTRY_TOTAL_SIZE, 500);
    put(32 + PAGEFILE_ENTRY_TOTAL_IN_USE, 50);
    std::memcpy(buffer, out.data(), needed);
    *returned = needed;
    return 0;
}

/// Writes a process list in SystemProcessInformation's layout: the idle process (no name, session 0) when
/// @p withIdle, then "Memory Compression" with compressionWorkingSet, compressionProcessId and
/// compressionSessionId when @p withCompression. @p needed bytes are asked for (the list's own size when 0).
LONG writeProcessList(PVOID buffer, ULONG length, PULONG returned, bool withIdle, bool withCompression, std::size_t needed)
{
    const auto& f = fake();
    constexpr std::wstring_view NAME = L"Memory Compression";
    constexpr std::size_t STRIDE = sizeof(SYSTEM_PROCESS_INFORMATION) + 64;
    const std::size_t entries = (withIdle ? 1U : 0U) + (withCompression ? 1U : 0U);
    if (needed == 0)
    {
        needed = STRIDE * entries;
    }
    *returned = static_cast<ULONG>(needed);
    if (length < needed)
    {
        return STATUS_INFO_LENGTH_MISMATCH_NT;
    }
    auto* at = static_cast<std::byte*>(buffer);
    if (withIdle)
    {
        SYSTEM_PROCESS_INFORMATION idle{};
        idle.NextEntryOffset = withCompression ? static_cast<ULONG>(STRIDE) : 0;
        idle.WorkingSetSize = 8;
        std::memcpy(at, &idle, sizeof(idle));
        at += STRIDE;
    }
    if (withCompression)
    {
        auto* name =
            reinterpret_cast<wchar_t*>(at + sizeof(SYSTEM_PROCESS_INFORMATION)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        std::memcpy(name, NAME.data(), NAME.size() * sizeof(wchar_t));
        SYSTEM_PROCESS_INFORMATION compression{};
        compression.ImageName.Length = static_cast<USHORT>(NAME.size() * sizeof(wchar_t));
        compression.ImageName.MaximumLength = compression.ImageName.Length;
        compression.ImageName.Buffer = name;
        compression.UniqueProcessId =
            reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(f.compressionProcessId)); // NOLINT(performance-no-int-to-ptr)
        compression.SessionId = f.compressionSessionId;
        compression.WorkingSetSize = static_cast<SIZE_T>(f.compressionWorkingSet);
        std::memcpy(at, &compression, sizeof(compression));
    }
    return 0;
}

/// The process list (#3363): the idle process, then "Memory Compression" when compressionWorkingSet is set.
/// Padded past the probe's first buffer when asked for a mismatch.
LONG processListQuery(PVOID buffer, ULONG length, PULONG returned)
{
    auto& f = fake();
    ++f.processListQueries;
    if (!f.processListAnswers)
    {
        return STATUS_ACCESS_DENIED_NT;
    }
    const std::size_t needed =
        f.processListMismatchFirst ? WindowsCommitPaging::PROCESS_INFORMATION_INITIAL_BYTES + sizeof(SYSTEM_PROCESS_INFORMATION) + 64 : 0;
    return writeProcessList(buffer, length, returned, true, f.compressionWorkingSet != 0, needed);
}

/// One session's process list (#3483): the request names the session and the buffer; the idle process is
/// in session 0, Memory Compression in compressionSessionId. An empty session writes nothing.
LONG sessionListQuery(PVOID request, ULONG length, PULONG returned)
{
    auto& f = fake();
    ++f.sessionListQueries;
    if (!f.sessionListAnswers || length != sizeof(WindowsCommitPaging::SessionProcessRequest))
    {
        return STATUS_ACCESS_DENIED_NT;
    }
    WindowsCommitPaging::SessionProcessRequest session{};
    std::memcpy(&session, request, sizeof(session));
    f.lastSessionQueried = session.sessionId;
    const bool withIdle = session.sessionId == 0;
    const bool withCompression = f.compressionWorkingSet != 0 && session.sessionId == f.compressionSessionId;
    if (!withIdle && !withCompression)
    {
        *returned = 0;
        return 0;
    }
    const LONG status = writeProcessList(session.buffer, session.bufferBytes, returned, withIdle, withCompression, 0);
    return status == STATUS_INFO_LENGTH_MISMATCH_NT && f.sessionListTooSmallAnswersBufferTooSmall ? STATUS_BUFFER_TOO_SMALL_NT : status;
}

LONG NTAPI fakeNtQuerySystemInformation(ULONG infoClass, PVOID buffer, ULONG length, PULONG returned)
{
    switch (infoClass)
    {
    case WindowsCommitPaging::SYSTEM_PROCESS_INFORMATION_CLASS:
        return processListQuery(buffer, length, returned);
    case WindowsCommitPaging::SYSTEM_SESSION_PROCESS_INFORMATION_CLASS:
        return sessionListQuery(buffer, length, returned);
    case PROCESSOR_PERFORMANCE_CLASS:
        ++fake().ntPerCoreCalls;
        if (fake().failNtPerCore)
        {
            return STATUS_ACCESS_DENIED_NT;
        }
        return fillPerformance(buffer, length, returned, fake().activePerGroup[0]);
    case PAGE_FILE_CLASS:
        return pageFileQuery(buffer, length, returned);
    case HYPERVISOR_CLASS: // Unanswered, as every unknown class is: the probe's SMBIOS fallback stays in use
        fake().hypervisorBuffer = std::bit_cast<std::uintptr_t>(buffer);
        return STATUS_ACCESS_DENIED_NT;
    case CODE_INTEGRITY_CLASS:
        return virtualizationQuery(infoClass, buffer);
    case ISOLATED_USER_MODE_CLASS:
        fake().isolatedUserModeBuffer = std::bit_cast<std::uintptr_t>(buffer);
        return virtualizationQuery(infoClass, buffer);
    default:
        return STATUS_ACCESS_DENIED_NT;
    }
}

LONG NTAPI fakeNtQuerySystemInformationEx(ULONG infoClass, PVOID input, ULONG /*inputLength*/, PVOID buffer, ULONG length, PULONG returned)
{
    auto& f = fake();
    ++f.exCalls;
    const USHORT group = *static_cast<const USHORT*>(input);
    if (infoClass != PROCESSOR_PERFORMANCE_CLASS || std::cmp_equal(group, f.failExGroup) || group >= f.activePerGroup.size())
    {
        return STATUS_ACCESS_DENIED_NT;
    }
    return fillPerformance(buffer, length, returned, f.activePerGroup[group] + f.extraCoresOnQuery);
}

BOOL WINAPI fakeGlobalMemoryStatusEx(LPMEMORYSTATUSEX status)
{
    fakeClock() += fake().memoryStatusTake;
    if (fake().failMemoryStatus)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    status->ullTotalPhys = 16ULL << 30U;
    status->ullAvailPhys = 6ULL << 30U;
    return TRUE;
}

BOOL WINAPI fakeGetPerformanceInfo(PPERFORMANCE_INFORMATION info, DWORD /*size*/)
{
    if (fake().failPerformanceInfo)
    {
        return FALSE;
    }
    info->SystemCache = 100;
    info->PageSize = PAGE_SIZE;
    info->CommitTotal = 200;
    info->CommitLimit = 800;
    info->CommitPeak = 300;
    return TRUE;
}

[[nodiscard]] WindowsSystemProbeFunctions fakeFunctions()
{
    WindowsSystemProbeFunctions fns;
    fns.cpu.getSystemTimes = &fakeGetSystemTimes;
    fns.cpu.getActiveProcessorGroupCount = &fakeGetActiveProcessorGroupCount;
    fns.cpu.getActiveProcessorCount = &fakeGetActiveProcessorCount;
    fns.cpu.getMaximumProcessorGroupCount = &fakeGetMaximumProcessorGroupCount;
    fns.cpu.getMaximumProcessorCount = &fakeGetMaximumProcessorCount;
    fns.cpu.getLogicalProcessorInformationEx = &fakeGetLogicalProcessorInformationEx;
    fns.cpu.getSystemInfo = &fakeGetSystemInfo;
    fns.nt.ntQuerySystemInformation = fake().ntAvailable ? &fakeNtQuerySystemInformation : nullptr;
    fns.nt.ntQuerySystemInformationEx = fake().exAvailable ? &fakeNtQuerySystemInformationEx : nullptr;
    fns.memory.globalMemoryStatusEx = &fakeGlobalMemoryStatusEx;
    fns.memory.getPerformanceInfo = &fakeGetPerformanceInfo;
    fns.network = WindowsSystemProbe::systemFunctions().network; // Faked in test_WindowsSystemProbeNetwork.cpp
    fns.now = &fakeNow;
    return fns;
}

[[nodiscard]] std::vector<std::size_t> coreIds(const SystemCounters& counters)
{
    std::vector<std::size_t> ids;
    std::ranges::transform(counters.cpuPerCore, std::back_inserter(ids), &CpuCounters::coreId);
    return ids;
}

class WindowsSystemProbeSeamsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeSystem{};
        fakeClock() = std::chrono::steady_clock::time_point{} + std::chrono::hours{1};
    }

    void TearDown() override
    {
        fake() = FakeSystem{};
    }

    /// A probe on the fakes as they are set now (the nullable NT exports are captured here).
    [[nodiscard]] static WindowsSystemProbe makeProbe()
    {
        return {2'000, nullptr, fakeFunctions()};
    }
};

} // namespace

TEST_F(WindowsSystemProbeSeamsTest, EveryCallWorkingFillsEveryValue)
{
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_EQ(counters.cpuTotal.idle, 1'000U);
    EXPECT_EQ(counters.cpuTotal.system, 2'000U);
    EXPECT_EQ(counters.cpuTotal.user, 500U);
    EXPECT_EQ(coreIds(counters), (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_EQ(counters.cpuPerCore[1].idle, 200U);
    EXPECT_EQ(counters.cpuPerCore[1].system, 50U);
    EXPECT_EQ(counters.cpuCoreCount, 4U);

    EXPECT_EQ(counters.memory.totalBytes, 16ULL << 30U);
    EXPECT_EQ(counters.memory.availableBytes, 6ULL << 30U);
    EXPECT_TRUE(counters.memory.hasAvailableBytes);
    EXPECT_EQ(counters.memory.cachedBytes, 100ULL * PAGE_SIZE);
    EXPECT_TRUE(counters.memory.hasCommitCharge);
    EXPECT_EQ(counters.memory.commitChargeBytes, 200ULL * PAGE_SIZE);
    EXPECT_EQ(counters.memory.swapTotalBytes, 1'500ULL * PAGE_SIZE);
    EXPECT_EQ(counters.memory.swapFreeBytes, 1'200ULL * PAGE_SIZE);

    const CpuDetails& details = counters.cpuDetails;
    EXPECT_EQ(details.sockets, 1U);
    EXPECT_EQ(details.logicalProcessors, 4U);
    EXPECT_EQ(details.l2CacheBytes, 1ULL << 20U);
    EXPECT_EQ(details.hvciEnabled, true);
    EXPECT_EQ(details.vbsRunning, true);
    EXPECT_EQ(fake().ntPerCoreCalls, 0) << "NtQuerySystemInformationEx answered, so no fallback";
}

TEST_F(WindowsSystemProbeSeamsTest, FailedAllGroupProcessorCountFallsBackToSystemInfo)
{
    fake().failAllGroupsCount = true;
    fake().failSystemTimes = true; // No per-core sample to update the count from
    auto probe = makeProbe();
    EXPECT_EQ(probe.read().cpuCoreCount, 4U);
}

TEST_F(WindowsSystemProbeSeamsTest, FailedSystemTimesLeavesCpuEmptyAndTheRestFilled)
{
    fake().failSystemTimes = true;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_EQ(counters.cpuTotal.total(), 0U);
    EXPECT_TRUE(counters.cpuPerCore.empty());
    EXPECT_EQ(counters.cpuCoreCount, 4U) << "the core count from construction is kept";
    EXPECT_EQ(counters.memory.totalBytes, 16ULL << 30U);
}

TEST_F(WindowsSystemProbeSeamsTest, IdleAboveKernelTimeGivesNoSystemTimeRatherThanWrapping)
{
    // GetSystemTimes' kernel time includes idle; a sample where it does not must not wrap to ~2^64
    // ticks of system time (#1719), as the per-core split already guards against.
    fake().idle = 5'000;
    fake().kernel = 3'000;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_EQ(counters.cpuTotal.system, 0U);
    EXPECT_EQ(counters.cpuTotal.idle, 5'000U);
    EXPECT_EQ(counters.cpuTotal.user, 500U);
}

TEST_F(WindowsSystemProbeSeamsTest, NegativeProcessorTimeReadsAsZero)
{
    fake().negativeIdleCore = 2;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    ASSERT_EQ(counters.cpuPerCore.size(), 4U);
    EXPECT_EQ(counters.cpuPerCore[2].idle, 0U);
    EXPECT_EQ(counters.cpuPerCore[2].system, 350U) << "all of the kernel time is busy when idle is unknown";
}

TEST_F(WindowsSystemProbeSeamsTest, FailedMemoryStatusLeavesMemoryEmpty)
{
    fake().failMemoryStatus = true;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_EQ(counters.memory.totalBytes, 0U);
    EXPECT_FALSE(counters.memory.hasAvailableBytes);
    EXPECT_FALSE(counters.memory.hasCommitCharge);
    EXPECT_EQ(counters.cpuPerCore.size(), 4U);
}

TEST_F(WindowsSystemProbeSeamsTest, FailedPerformanceInfoLeavesCachedAndCommitUnknown)
{
    fake().failPerformanceInfo = true;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_EQ(counters.memory.cachedBytes, 0U);
    EXPECT_FALSE(counters.memory.hasCommitCharge);
    EXPECT_EQ(counters.memory.commitChargeBytes, 0U);
    EXPECT_EQ(counters.memory.totalBytes, 16ULL << 30U);
    EXPECT_EQ(counters.memory.swapTotalBytes, 1'500ULL * PAGE_SIZE);
}

TEST_F(WindowsSystemProbeSeamsTest, PageFileQueryFailureOrNoPageFileGivesZeroSwap)
{
    for (const bool failed : {true, false})
    {
        fake().failPageFile = failed;
        fake().noPageFile = !failed;
        auto probe = makeProbe();
        const SystemCounters counters = probe.read();
        EXPECT_EQ(counters.memory.swapTotalBytes, 0U) << failed;
        EXPECT_EQ(counters.memory.swapFreeBytes, 0U) << failed;
        EXPECT_EQ(counters.memory.totalBytes, 16ULL << 30U) << failed;
    }
}

TEST_F(WindowsSystemProbeSeamsTest, PageFileListLongerThanTheFirstBufferIsReadInFull)
{
    fake().pageFileMismatchFirst = true;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_EQ(counters.memory.swapTotalBytes, 1'500ULL * PAGE_SIZE);
}

// #3363: compressed memory is the Memory Compression process's working set. #3483: the first sample finds
// it in the whole process list, grown into a buffer kept between samples; each later one reads it, at the
// same interval, from its session's list alone.
TEST_F(WindowsSystemProbeSeamsTest, CompressedMemoryIsTheMemoryCompressionProcesssWorkingSet)
{
    fake().compressionWorkingSet = 300ULL << 20U;
    fake().compressionSessionId = 2; // whichever session the snapshot says, not an assumed 0
    fake().processListMismatchFirst = true;
    auto probe = makeProbe();
    SystemCounters counters = probe.read();
    EXPECT_TRUE(counters.memory.hasMemoryCompression);
    EXPECT_EQ(counters.memory.memoryCompressionBytes, 300ULL << 20U);
    EXPECT_EQ(fake().processListQueries, 2) << "the first buffer was too small, so it grew once";
    EXPECT_EQ(fake().sessionListQueries, 0);

    fake().compressionWorkingSet = 310ULL << 20U;
    counters = probe.read();
    EXPECT_TRUE(counters.memory.hasMemoryCompression);
    EXPECT_EQ(counters.memory.memoryCompressionBytes, 310ULL << 20U) << "read again each sample";
    EXPECT_EQ(fake().processListQueries, 2) << "no whole snapshot once the process is known";
    EXPECT_EQ(fake().sessionListQueries, 1);
    EXPECT_EQ(fake().lastSessionQueried, 2U);
}

// #3483: after the first sample, no sample takes the whole process snapshot while the process stays.
TEST_F(WindowsSystemProbeSeamsTest, CompressedMemoryTakesNoWholeSnapshotPerSampleAfterTheFirst)
{
    fake().compressionWorkingSet = 300ULL << 20U;
    auto probe = makeProbe();
    for (int sample = 0; sample < 20; ++sample)
    {
        fake().compressionWorkingSet = (300ULL + static_cast<std::uint64_t>(sample)) << 20U;
        fakeClock() += std::chrono::seconds{1};
        const SystemCounters counters = probe.read();
        ASSERT_TRUE(counters.memory.hasMemoryCompression) << "sample " << sample;
        EXPECT_EQ(counters.memory.memoryCompressionBytes, (300ULL + static_cast<std::uint64_t>(sample)) << 20U) << "sample " << sample;
    }
    EXPECT_EQ(fake().processListQueries, 1);
    EXPECT_EQ(fake().sessionListQueries, 19);
}

// #3483: a process gone from its session's list (compression restarted it under a new id) is searched
// for again in the same sample, so the value doesn't skip one, and the new id is read after that.
TEST_F(WindowsSystemProbeSeamsTest, CompressedMemoryFindsTheProcessAgainWhenItsGone)
{
    fake().compressionWorkingSet = 300ULL << 20U;
    auto probe = makeProbe();
    ASSERT_TRUE(probe.read().memory.hasMemoryCompression);

    fake().compressionProcessId = 9100; // a new process
    fake().compressionWorkingSet = 50ULL << 20U;
    SystemCounters counters = probe.read();
    EXPECT_TRUE(counters.memory.hasMemoryCompression);
    EXPECT_EQ(counters.memory.memoryCompressionBytes, 50ULL << 20U);
    EXPECT_EQ(fake().sessionListQueries, 1) << "the old id was looked for in the session list";
    EXPECT_EQ(fake().processListQueries, 2) << "then found again in the whole snapshot";

    fake().compressionWorkingSet = 60ULL << 20U;
    counters = probe.read();
    EXPECT_EQ(counters.memory.memoryCompressionBytes, 60ULL << 20U);
    EXPECT_EQ(fake().processListQueries, 2);
    EXPECT_EQ(fake().sessionListQueries, 2);
}

// #3363: with no Memory Compression process (compression off) the value is hidden. #3483: the whole
// snapshot then looks for one only every MEMORY_COMPRESSION_SEARCH_INTERVAL_SECONDS, so compression
// turned on is still found; turned off again, it is hidden at once.
TEST_F(WindowsSystemProbeSeamsTest, CompressedMemoryIsHiddenWhenOffAndSearchedForOnTheSearchInterval)
{
    auto probe = makeProbe();
    SystemCounters counters = probe.read();
    EXPECT_FALSE(counters.memory.hasMemoryCompression);
    EXPECT_EQ(counters.memory.memoryCompressionBytes, 0U);
    EXPECT_EQ(fake().processListQueries, 1);

    const auto interval = std::chrono::seconds(Domain::Sampling::MEMORY_COMPRESSION_SEARCH_INTERVAL_SECONDS);
    fake().compressionWorkingSet = 300ULL << 20U; // turned on
    fakeClock() += interval - std::chrono::seconds{1};
    EXPECT_FALSE(probe.read().memory.hasMemoryCompression) << "not searched for before the interval";
    EXPECT_EQ(fake().processListQueries, 1);
    EXPECT_EQ(fake().sessionListQueries, 0);

    fakeClock() += std::chrono::seconds{1};
    counters = probe.read();
    EXPECT_TRUE(counters.memory.hasMemoryCompression);
    EXPECT_EQ(counters.memory.memoryCompressionBytes, 300ULL << 20U);
    EXPECT_EQ(fake().processListQueries, 2);

    fake().compressionWorkingSet = 0; // turned off
    counters = probe.read();
    EXPECT_FALSE(counters.memory.hasMemoryCompression);
    EXPECT_EQ(counters.memory.memoryCompressionBytes, 0U);
    EXPECT_EQ(fake().processListQueries, 3) << "gone from the session list, so the whole snapshot was searched";
    EXPECT_FALSE(probe.read().memory.hasMemoryCompression);
    EXPECT_EQ(fake().processListQueries, 3) << "and not again before the interval";
}

// #3363: no process list, or no NtQuerySystemInformation, hides the value and nothing else. #3483: a
// session list that fails falls back to the whole snapshot each sample, at the same interval.
TEST_F(WindowsSystemProbeSeamsTest, CompressedMemoryIsHiddenOnFailureAndFallsBackToTheWholeSnapshot)
{
    fake().compressionWorkingSet = 300ULL << 20U;
    fake().processListAnswers = false;
    auto probe = makeProbe();
    SystemCounters counters = probe.read();
    EXPECT_FALSE(counters.memory.hasMemoryCompression);
    EXPECT_EQ(counters.memory.totalBytes, 16ULL << 30U) << "the rest of memory is still read";

    fake().ntAvailable = false;
    auto withoutNt = makeProbe();
    EXPECT_FALSE(withoutNt.read().memory.hasMemoryCompression);

    fake() = FakeSystem{};
    fake().compressionWorkingSet = 300ULL << 20U;
    fake().sessionListAnswers = false;
    auto withoutSessions = makeProbe();
    for (int sample = 0; sample < 3; ++sample)
    {
        fake().compressionWorkingSet = (300ULL + static_cast<std::uint64_t>(sample)) << 20U;
        counters = withoutSessions.read();
        ASSERT_TRUE(counters.memory.hasMemoryCompression) << "sample " << sample;
        EXPECT_EQ(counters.memory.memoryCompressionBytes, (300ULL + static_cast<std::uint64_t>(sample)) << 20U);
    }
    EXPECT_EQ(fake().sessionListQueries, 1) << "tried once";
    EXPECT_EQ(fake().processListQueries, 3) << "then the whole snapshot each sample";
}

// #3483: the session list grows its buffer on STATUS_BUFFER_TOO_SMALL as well as on
// STATUS_INFO_LENGTH_MISMATCH, as it may answer either when the request's buffer is too small.
TEST_F(WindowsSystemProbeSeamsTest, SessionProcessListGrowsOnBufferTooSmall)
{
    fake().compressionWorkingSet = 300ULL << 20U;
    fake().sessionListTooSmallAnswersBufferTooSmall = true;
    std::vector<std::byte> buffer;
    const std::optional<std::size_t> written =
        WindowsCommitPaging::querySessionProcessesInto(&fakeNtQuerySystemInformation, 0, buffer, sizeof(SYSTEM_PROCESS_INFORMATION));
    EXPECT_EQ(fake().sessionListQueries, 2) << "grown once";
    ASSERT_TRUE(written.has_value());
    const std::optional<WindowsCommitPaging::MemoryCompressionProcess> process = WindowsCommitPaging::findMemoryCompressionProcess(
        std::span<const std::byte>(buffer.data(), written.value_or(0)), fake().compressionProcessId);
    ASSERT_TRUE(process.has_value());
    EXPECT_EQ(process.value_or(WindowsCommitPaging::MemoryCompressionProcess{}).workingSet, 300ULL << 20U);
    EXPECT_EQ(process.value_or(WindowsCommitPaging::MemoryCompressionProcess{.sessionId = 1}).sessionId, 0U);
}

TEST_F(WindowsSystemProbeSeamsTest, WithoutNtQuerySystemInformationExTheOneGroupQueryIsUsed)
{
    fake().exAvailable = false;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_EQ(coreIds(counters), (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_EQ(fake().ntPerCoreCalls, 1);
}

TEST_F(WindowsSystemProbeSeamsTest, FailedGroupQueryOnOneGroupFallsBackToTheOneGroupQuery)
{
    fake().failExGroup = 0;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_EQ(coreIds(counters), (std::vector<std::size_t>{0, 1, 2, 3}));
    EXPECT_EQ(fake().ntPerCoreCalls, 1);
}

TEST_F(WindowsSystemProbeSeamsTest, FailedOrMissingPerCoreQueriesLeavePerCoreEmpty)
{
    fake().failExGroup = 0;
    fake().failNtPerCore = true;
    {
        auto probe = makeProbe();
        const SystemCounters counters = probe.read();
        EXPECT_TRUE(counters.cpuPerCore.empty());
        EXPECT_EQ(counters.cpuTotal.system, 2'000U) << "the Total comes from GetSystemTimes";
        EXPECT_EQ(counters.cpuCoreCount, 4U);
    }

    fake().exAvailable = false;
    fake().ntAvailable = false;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();
    EXPECT_TRUE(counters.cpuPerCore.empty());
    EXPECT_EQ(counters.cpuTotal.system, 2'000U);
    EXPECT_FALSE(counters.cpuDetails.hvciEnabled.has_value()) << "no NtQuerySystemInformation, no virtualization status";
    EXPECT_EQ(counters.memory.swapTotalBytes, 0U);
}

TEST_F(WindowsSystemProbeSeamsTest, ProcessorsAddedBetweenSizingAndQueryAreAllRead)
{
    fake().extraCoresOnQuery = 2; // The group reports 4 when sized, 6 when queried
    fake().maxPerGroup = {8};
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_EQ(coreIds(counters), (std::vector<std::size_t>{0, 1, 2, 3, 4, 5}));
    EXPECT_EQ(counters.cpuCoreCount, 6U);
}

TEST_F(WindowsSystemProbeSeamsTest, SeveralGroupsAreNumberedFromEachGroupsFirstIdAndSummed)
{
    fake().maxPerGroup = {4, 4};
    fake().activePerGroup = {2, 3};
    fake().activeGroups = 2;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_EQ(coreIds(counters), (std::vector<std::size_t>{0, 1, 4, 5, 6}));
    // The Total is the all-group sum, not GetSystemTimes' (#1107)
    EXPECT_EQ(counters.cpuTotal.idle, 100U + 200U + 100U + 200U + 300U);
    EXPECT_EQ(counters.cpuTotal.system, 5U * 50U);
}

TEST_F(WindowsSystemProbeSeamsTest, FailedGroupRepeatsTheLastAllGroupTotalAndNoPerCore)
{
    fake().maxPerGroup = {4, 4};
    fake().activePerGroup = {2, 2};
    fake().activeGroups = 2;
    fake().failExGroup = 1;
    auto probe = makeProbe();

    // Before any complete read: no per-core, and a zeroed Total, never the one-group one
    SystemCounters counters = probe.read();
    EXPECT_TRUE(counters.cpuPerCore.empty());
    EXPECT_EQ(counters.cpuTotal.total(), 0U);
    EXPECT_EQ(fake().ntPerCoreCalls, 0) << "no one-group fallback on several groups";

    fake().failExGroup = -1;
    const SystemCounters complete = probe.read();
    ASSERT_EQ(complete.cpuPerCore.size(), 4U);

    fake().failExGroup = 1;
    counters = probe.read();
    EXPECT_TRUE(counters.cpuPerCore.empty());
    EXPECT_EQ(counters.cpuTotal.idle, complete.cpuTotal.idle);
    EXPECT_EQ(counters.cpuTotal.system, complete.cpuTotal.system);
    EXPECT_EQ(counters.cpuCoreCount, 4U) << "a failed sample keeps the core count";
}

TEST_F(WindowsSystemProbeSeamsTest, GroupBeyondTheBootTimeTableIsAFailedRead)
{
    fake().maxPerGroup = {4}; // One group at boot
    fake().activePerGroup = {2, 2};
    fake().activeGroups = 2;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_TRUE(counters.cpuPerCore.empty());
    EXPECT_EQ(counters.cpuTotal.total(), 0U);
}

TEST_F(WindowsSystemProbeSeamsTest, FailedTopologyReadLeavesTopologyUnknown)
{
    for (const bool failSizing : {true, false})
    {
        fake().failTopologySizing = failSizing;
        fake().failTopologyRead = !failSizing;
        auto probe = makeProbe();
        const SystemCounters counters = probe.read();
        EXPECT_FALSE(counters.cpuDetails.sockets.has_value()) << failSizing;
        EXPECT_FALSE(counters.cpuDetails.logicalProcessors.has_value()) << failSizing;
        EXPECT_FALSE(counters.cpuDetails.l2CacheBytes.has_value()) << failSizing;
        EXPECT_EQ(counters.cpuDetails.hvciEnabled, true) << failSizing;
        EXPECT_EQ(counters.cpuPerCore.size(), 4U) << failSizing;
    }
}

TEST_F(WindowsSystemProbeSeamsTest, FailedVirtualizationQueriesLeaveTheirStatusUnknown)
{
    fake().failCodeIntegrity = true;
    fake().failIsolatedUserMode = true;
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    EXPECT_FALSE(counters.cpuDetails.hvciEnabled.has_value());
    EXPECT_FALSE(counters.cpuDetails.vbsRunning.has_value());
    EXPECT_TRUE(counters.cpuDetails.virtualizationFirmwareEnabled.has_value()) << "the feature flags need no query";
}

TEST_F(WindowsSystemProbeSeamsTest, VirtualizationQueryBuffersAreAlignedForTheirUlonglong)
{
    // SYSTEM_ISOLATED_USER_MODE_INFORMATION and SYSTEM_HYPERVISOR_QUERY_INFORMATION each end in a
    // ULONGLONG, and the kernel rejects a buffer not aligned for it with STATUS_DATATYPE_MISALIGNMENT,
    // which the probe would read as "unknown" (#2746).
    auto probe = makeProbe();
    const SystemCounters counters = probe.read();

    ASSERT_EQ(counters.cpuDetails.vbsRunning, true);
    ASSERT_NE(fake().isolatedUserModeBuffer, 0U);
    EXPECT_EQ(fake().isolatedUserModeBuffer % alignof(ULONGLONG), 0U);
#if defined(_M_X64) || defined(__x86_64__)
    EXPECT_EQ(fake().hypervisorBuffer, 0U) << "x64 reads CPUID's hypervisor bit, not SystemHypervisorInformation";
#else
    ASSERT_NE(fake().hypervisorBuffer, 0U);
    EXPECT_EQ(fake().hypervisorBuffer % alignof(ULONGLONG), 0U);
#endif
}

TEST_F(WindowsSystemProbeSeamsTest, ProcessorGoingOfflineAndBackRereadsTheDetails)
{
    auto probe = makeProbe();
    ASSERT_EQ(probe.read().cpuDetails.logicalProcessors, 4U);
    const int readsAtStart = fake().topologyReads;

    // Processor 3 goes offline: the sample and the topology agree on three
    fake().activePerGroup = {3};
    fake().topologyIds = {0, 1, 2};
    SystemCounters counters = probe.read();
    EXPECT_EQ(counters.cpuCoreCount, 3U);
    EXPECT_EQ(counters.cpuDetails.logicalProcessors, 3U);
    EXPECT_EQ(fake().topologyReads, readsAtStart + 1);

    // An unchanged set is not read again
    counters = probe.read();
    EXPECT_EQ(fake().topologyReads, readsAtStart + 1);

    // Back online
    fake().activePerGroup = {4};
    fake().topologyIds = {0, 1, 2, 3};
    counters = probe.read();
    EXPECT_EQ(counters.cpuCoreCount, 4U);
    EXPECT_EQ(counters.cpuDetails.logicalProcessors, 4U);
}

TEST_F(WindowsSystemProbeSeamsTest, TopologyThatDisagreesWithTheSampleKeepsThePreviousDetails)
{
    auto probe = makeProbe();
    ASSERT_EQ(probe.read().cpuDetails.logicalProcessors, 4U);

    // The per-core read sees three processors, the topology still four (a race): the details stay
    // as they were and are retried a bounded number of times
    fake().activePerGroup = {3};
    const int readsAtStart = fake().topologyReads;
    SystemCounters counters;
    for (unsigned attempt = 0; attempt < CpuTopology::MAX_INCONSISTENT_CPU_DETAIL_READS + 2; ++attempt)
    {
        counters = probe.read();
    }
    EXPECT_EQ(counters.cpuCoreCount, 3U);
    EXPECT_EQ(counters.cpuDetails.logicalProcessors, 4U);
    EXPECT_EQ(fake().topologyReads, readsAtStart + static_cast<int>(CpuTopology::MAX_INCONSISTENT_CPU_DETAIL_READS));
}

// #2731: each sub-read of a pass is timed on the probe's clock, and the slowest is kept by name, so an
// overrun can name "pdh-freq" or "ifTable" rather than "system". The fakes advance the clock in place of
// the real calls' time.

TEST_F(WindowsSystemProbeSeamsTest, TheSlowestSubReadIsNamedWithItsDuration)
{
    fake().memoryStatusTake = std::chrono::milliseconds{40};
    auto probe = makeProbe();
    EXPECT_TRUE(probe.lastReadTiming().slowestSubRead.empty()) << "nothing read yet";

    (void) probe.read();
    SystemReadTiming timing = probe.lastReadTiming();
    EXPECT_EQ(timing.slowestSubRead, "memory");
    EXPECT_EQ(timing.slowest, std::chrono::milliseconds{40});
    EXPECT_EQ(timing.total, std::chrono::milliseconds{40}) << "the other sub-reads take no fake time";

    // The next pass stands on its own: the CPU read is now the slow one
    fake().systemTimesTake = std::chrono::milliseconds{150};
    (void) probe.read();
    timing = probe.lastReadTiming();
    EXPECT_EQ(timing.slowestSubRead, "cpu");
    EXPECT_EQ(timing.slowest, std::chrono::milliseconds{150});
    EXPECT_EQ(timing.total, std::chrono::milliseconds{190});
    EXPECT_GE(timing.slowest, std::chrono::milliseconds{Domain::Sampling::SYSTEM_PROBE_SLOW_SUB_READ_MS}) << "and is logged by name";

    // A pass with no time taken still names a sub-read, so the timing never reads as unset
    fake().systemTimesTake = std::chrono::milliseconds{0};
    fake().memoryStatusTake = std::chrono::milliseconds{0};
    (void) probe.read();
    timing = probe.lastReadTiming();
    EXPECT_FALSE(timing.slowestSubRead.empty());
    EXPECT_EQ(timing.slowest, std::chrono::steady_clock::duration{0});
    EXPECT_EQ(timing.total, std::chrono::steady_clock::duration{0});
}

TEST_F(WindowsSystemProbeSeamsTest, SystemFunctionsAreAllSet)
{
    const WindowsSystemProbeFunctions fns = WindowsSystemProbe::systemFunctions();
    EXPECT_NE(fns.now, nullptr);
    EXPECT_NE(fns.cpu.getSystemTimes, nullptr);
    EXPECT_NE(fns.cpu.getActiveProcessorGroupCount, nullptr);
    EXPECT_NE(fns.cpu.getActiveProcessorCount, nullptr);
    EXPECT_NE(fns.cpu.getMaximumProcessorGroupCount, nullptr);
    EXPECT_NE(fns.cpu.getMaximumProcessorCount, nullptr);
    EXPECT_NE(fns.cpu.getLogicalProcessorInformationEx, nullptr);
    EXPECT_NE(fns.cpu.getSystemInfo, nullptr);
    EXPECT_NE(fns.nt.ntQuerySystemInformation, nullptr);
    EXPECT_NE(fns.memory.globalMemoryStatusEx, nullptr);
    EXPECT_NE(fns.memory.getPerformanceInfo, nullptr);
    EXPECT_NE(fns.network.getIfTable2, nullptr);
    EXPECT_NE(fns.network.freeMibTable, nullptr);
    EXPECT_NE(fns.network.readAdapterDeviceInstanceId, nullptr);
    EXPECT_NE(fns.network.devicePresence, nullptr);
}

} // namespace Platform
// NOLINTEND(misc-include-cleaner,cppcoreguidelines-pro-type-union-access,readability-non-const-parameter)
