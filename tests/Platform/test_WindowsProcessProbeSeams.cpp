/// @file test_WindowsProcessProbeSeams.cpp
/// @brief WindowsProcessProbe against fake Win32/NT calls (#1718): the EStats network counters,
/// processor-group affinity and per-process detail failures the real APIs can't be made to show.
/// Every fake is side-effect free: the only real calls are CreateEventW for handles the probe may
/// close, and the probe's own RAM query. No process is opened, changed or ended.

#include "Platform/CpuAffinity.h"
#include "Platform/ProcessTypes.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcessActionsMath.h"
#include "Platform/Windows/WindowsProcessProbe.h"
#include "Platform/Windows/WindowsProcessProbeMath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winternl.h>
// clang-format on

// NOLINTBEGIN(misc-include-cleaner,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr,cppcoreguidelines-pro-type-union-access,readability-non-const-parameter)
// Win32 types; the fakes fill OS-defined layouts (IDs stored as HANDLEs, unions) and keep the
// exact Win32 signatures.
namespace Platform
{
namespace
{

constexpr DWORD PID = 1234;
constexpr LONG STATUS_ACCESS_DENIED_NT = static_cast<LONG>(0xC0000022L);
constexpr LONG STATUS_INFO_LENGTH_MISMATCH_NT = static_cast<LONG>(0xC0000004L);

struct FakeProcess
{
    DWORD pid = PID;
    LONGLONG createTime = 133'000'000'000'000'000;
    std::wstring name = L"app.exe";
    std::vector<DWORD> threadIds;
};

struct FakeTcpRow
{
    DWORD state = MIB_TCP_STATE_ESTAB;
    DWORD localPort = 0; // Identifies the row to the fake EStats reads
    DWORD pid = 0;
    ULONG64 bytesIn = 0;
    ULONG64 bytesOut = 0;
};

/// What every fake returns, and what the probe asked of them. Grouped by area, not packed.
struct FakeWin // NOLINT(clang-analyzer-optin.performance.Padding)
{
    std::vector<FakeProcess> processes{FakeProcess{}};
    // Processor groups
    std::vector<DWORD> groupMaxima{8};
    std::vector<KAFFINITY> activeMasks{0xFF};
    bool failLogicalProcessorInfo = false;
    std::uint32_t buildNumber = 19045; // Before threads span groups by default
    // Token
    bool elevated = false;
    bool failOpenToken = false;
    bool failTokenSize = false;
    bool failTokenRead = false;
    bool failLookup = false;
    std::wstring user = L"alice";
    // Process
    bool failOpenProcess = false;
    int openProcessCalls = 0;
    HANDLE queryInfoHandle = nullptr; // The last PROCESS_QUERY_INFORMATION open
    DWORD priorityClass = HIGH_PRIORITY_CLASS;
    std::wstring imagePath = L"C:\\fake\\app.exe";
    bool failImagePath = false;
    std::wstring commandLine = L"app.exe --flag";
    bool failStatus = false;
    bool failCommandLine = false;
    ULONG statusFlags = 0x20; // PEBI_IS_BACKGROUND
    bool failGdiOnLimitedHandle = false;
    bool failGdi = false;
    DWORD gdiObjects = 7;
    DWORD userObjects = 2;
    bool failAffinityMask = false;
    DWORD_PTR processMask = 0b101;
    std::vector<USHORT> processGroups{0};
    bool failGroupAffinity = false;
    bool groupAffinityAlwaysTooSmall = false;
    std::map<DWORD, GROUP_AFFINITY> threadAffinities; // By thread ID, read back in ID order
    std::size_t nextThread = 0;
    DWORD threadOwnerPid = PID;
    // EStats
    bool estatsExported = true;
    bool ipv6Exported = true;
    int loadCalls = 0;
    DWORD dummySetStatus = ERROR_NOT_FOUND;
    DWORD readStatus = NO_ERROR;
    int enableCalls = 0;
    std::vector<FakeTcpRow> rows4;
    std::vector<FakeTcpRow> rows6;
    bool failTableSize = false;
    int tableFillsTooSmall = 0; // Fills that report the table grew meanwhile
    int table6Calls = 0;
    bool failSnapshot = false;
};

FakeWin& fake()
{
    static FakeWin state;
    return state;
}

HANDLE newHandle()
{
    return CreateEventW(nullptr, TRUE, FALSE, nullptr); // A real handle the probe may close
}

// --- System ---------------------------------------------------------------------------------

LONG NTAPI fakeNtQuerySystemInformation(ULONG /*infoClass*/, PVOID buffer, ULONG length, PULONG returnLength)
{
    if (fake().failSnapshot)
    {
        return STATUS_ACCESS_DENIED_NT;
    }
    const auto entrySize = [](const FakeProcess& p)
    {
        const std::size_t bytes = sizeof(SYSTEM_PROCESS_INFORMATION) + (p.threadIds.size() * sizeof(SYSTEM_THREAD_INFORMATION)) +
                                  ((p.name.size() + 1) * sizeof(wchar_t));
        return (bytes + 7U) & ~std::size_t{7};
    };
    std::size_t needed = 0;
    for (const FakeProcess& p : fake().processes)
    {
        needed += entrySize(p);
    }
    *returnLength = static_cast<ULONG>(needed);
    if (length < needed)
    {
        return STATUS_INFO_LENGTH_MISMATCH_NT;
    }
    auto* out = static_cast<std::byte*>(buffer);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < fake().processes.size(); ++i)
    {
        const FakeProcess& p = fake().processes[i];
        SYSTEM_PROCESS_INFORMATION info{};
        info.NextEntryOffset = (i + 1 < fake().processes.size()) ? static_cast<ULONG>(entrySize(p)) : 0;
        info.NumberOfThreads = static_cast<ULONG>(p.threadIds.size());
        // CreateTime is in Reserved1, after WorkingSetPrivateSize, HardFaultCount,
        // NumberOfThreadsHighWatermark and CycleTime: the layout WindowsProcessProbe.cpp's
        // SystemProcessInfo static_asserts against this structure.
        constexpr std::size_t CREATE_TIME_IN_RESERVED1 = 24;
        std::memcpy(&info.Reserved1[CREATE_TIME_IN_RESERVED1], &p.createTime, sizeof(p.createTime));
        info.BasePriority = 8;
        info.UniqueProcessId = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(p.pid));
        std::size_t at = offset + sizeof(SYSTEM_PROCESS_INFORMATION);
        for (const DWORD tid : p.threadIds)
        {
            SYSTEM_THREAD_INFORMATION thread{};
            thread.ClientId.UniqueThread = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(tid));
            std::memcpy(out + at, &thread, sizeof(thread));
            at += sizeof(thread);
        }
        std::memcpy(out + at, p.name.c_str(), (p.name.size() + 1) * sizeof(wchar_t));
        info.ImageName.Buffer = reinterpret_cast<PWSTR>(out + at);
        info.ImageName.Length = static_cast<USHORT>(p.name.size() * sizeof(wchar_t));
        std::memcpy(out + offset, &info, sizeof(info));
        offset += entrySize(p);
    }
    return 0;
}

WORD WINAPI fakeGetMaximumProcessorGroupCount()
{
    return static_cast<WORD>(fake().groupMaxima.size());
}

DWORD WINAPI fakeGetMaximumProcessorCount(WORD group)
{
    return fake().groupMaxima.at(group);
}

BOOL WINAPI fakeGetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP /*relation*/,
                                                 PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer,
                                                 PDWORD bytes)
{
    constexpr std::size_t INFO_OFFSET = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Group) + offsetof(GROUP_RELATIONSHIP, GroupInfo);
    const std::size_t needed = INFO_OFFSET + (fake().activeMasks.size() * sizeof(PROCESSOR_GROUP_INFO));
    if (fake().failLogicalProcessorInfo || buffer == nullptr || *bytes < needed)
    {
        *bytes = fake().failLogicalProcessorInfo ? 0 : static_cast<DWORD>(needed);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    auto* out = reinterpret_cast<std::byte*>(buffer);
    std::memset(out, 0, needed);
    buffer->Relationship = RelationGroup;
    buffer->Size = static_cast<DWORD>(needed);
    buffer->Group.ActiveGroupCount = static_cast<WORD>(fake().activeMasks.size());
    for (std::size_t g = 0; g < fake().activeMasks.size(); ++g)
    {
        PROCESSOR_GROUP_INFO info{};
        info.ActiveProcessorMask = fake().activeMasks[g];
        std::memcpy(out + INFO_OFFSET + (g * sizeof(info)), &info, sizeof(info));
    }
    return TRUE;
}

std::uint32_t fakeWindowsBuildNumber()
{
    return fake().buildNumber;
}

// --- Token ----------------------------------------------------------------------------------

BOOL WINAPI fakeOpenProcessToken(HANDLE /*process*/, DWORD /*access*/, PHANDLE token)
{
    if (fake().failOpenToken)
    {
        return FALSE;
    }
    *token = newHandle();
    return TRUE;
}

BOOL WINAPI fakeGetTokenInformation(HANDLE /*token*/, TOKEN_INFORMATION_CLASS infoClass, LPVOID info, DWORD length, PDWORD returned)
{
    if (infoClass == TokenElevation)
    {
        TOKEN_ELEVATION elevation{.TokenIsElevated = fake().elevated ? 1U : 0U};
        std::memcpy(info, &elevation, sizeof(elevation));
        return TRUE;
    }
    constexpr DWORD NEEDED = sizeof(TOKEN_USER) + 16;
    if (fake().failTokenSize)
    {
        *returned = 0;
        return FALSE;
    }
    if (info == nullptr || length < NEEDED)
    {
        *returned = NEEDED;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (fake().failTokenRead)
    {
        return FALSE;
    }
    TOKEN_USER user{};
    user.User.Sid = static_cast<std::byte*>(info) + sizeof(TOKEN_USER);
    std::memcpy(info, &user, sizeof(user));
    return TRUE;
}

BOOL WINAPI fakeLookupAccountSidW(
    LPCWSTR /*system*/, PSID /*sid*/, LPWSTR name, LPDWORD nameLength, LPWSTR /*domain*/, LPDWORD /*domainLength*/, PSID_NAME_USE /*use*/)
{
    if (fake().failLookup || *nameLength <= fake().user.size())
    {
        return FALSE;
    }
    std::memcpy(name, fake().user.c_str(), (fake().user.size() + 1) * sizeof(wchar_t));
    return TRUE;
}

// --- Process --------------------------------------------------------------------------------

HANDLE WINAPI fakeOpenProcess(DWORD access, BOOL /*inherit*/, DWORD /*pid*/)
{
    ++fake().openProcessCalls;
    if (fake().failOpenProcess)
    {
        return nullptr;
    }
    HANDLE handle = newHandle();
    if (access == PROCESS_QUERY_INFORMATION)
    {
        fake().queryInfoHandle = handle;
    }
    return handle;
}

DWORD WINAPI fakeGetPriorityClass(HANDLE /*process*/)
{
    return fake().priorityClass;
}

BOOL WINAPI fakeQueryFullProcessImageNameW(HANDLE /*process*/, DWORD /*flags*/, LPWSTR path, PDWORD size)
{
    if (fake().failImagePath)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (*size <= fake().imagePath.size())
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(path, fake().imagePath.c_str(), (fake().imagePath.size() + 1) * sizeof(wchar_t));
    *size = static_cast<DWORD>(fake().imagePath.size());
    return TRUE;
}

/// ProcessExtendedBasicInformation's layout, as the probe reads it.
struct ExtendedBasicInformation
{
    SIZE_T size;
    PROCESS_BASIC_INFORMATION basicInfo;
    ULONG flags;
};

LONG NTAPI fakeNtQueryInformationProcess(HANDLE /*process*/, ULONG infoClass, PVOID buffer, ULONG length, PULONG returned)
{
    constexpr ULONG EXTENDED_BASIC = 64;
    if (infoClass == EXTENDED_BASIC)
    {
        if (fake().failStatus)
        {
            return STATUS_ACCESS_DENIED_NT;
        }
        ExtendedBasicInformation info{};
        info.flags = fake().statusFlags;
        std::memcpy(buffer, &info, std::min<std::size_t>(length, sizeof(info)));
        return 0;
    }
    if (fake().failCommandLine)
    {
        return STATUS_ACCESS_DENIED_NT;
    }
    const std::size_t textBytes = fake().commandLine.size() * sizeof(wchar_t);
    const auto needed = static_cast<ULONG>(sizeof(UNICODE_STRING) + textBytes);
    *returned = needed;
    if (length < needed)
    {
        return STATUS_INFO_LENGTH_MISMATCH_NT;
    }
    auto* out = static_cast<std::byte*>(buffer);
    UNICODE_STRING text{};
    text.Length = static_cast<USHORT>(textBytes);
    text.MaximumLength = text.Length;
    text.Buffer = reinterpret_cast<PWSTR>(out + sizeof(UNICODE_STRING));
    std::memcpy(out, &text, sizeof(text));
    std::memcpy(out + sizeof(UNICODE_STRING), fake().commandLine.data(), textBytes);
    return 0;
}

DWORD WINAPI fakeGetGuiResources(HANDLE process, DWORD flags)
{
    if (fake().failGdi || (fake().failGdiOnLimitedHandle && process != fake().queryInfoHandle))
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    return (flags == GR_GDIOBJECTS) ? fake().gdiObjects : fake().userObjects;
}

BOOL WINAPI fakeGetProcessAffinityMask(HANDLE /*process*/, PDWORD_PTR processMask, PDWORD_PTR systemMask)
{
    if (fake().failAffinityMask)
    {
        return FALSE;
    }
    *processMask = fake().processMask;
    *systemMask = fake().processMask;
    return TRUE;
}

BOOL WINAPI fakeGetProcessGroupAffinity(HANDLE /*process*/, PUSHORT count, PUSHORT groups)
{
    if (fake().failGroupAffinity)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (fake().groupAffinityAlwaysTooSmall || *count < fake().processGroups.size())
    {
        *count = static_cast<USHORT>(fake().processGroups.size() + (fake().groupAffinityAlwaysTooSmall ? 1U : 0U));
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::ranges::copy(fake().processGroups, groups);
    *count = static_cast<USHORT>(fake().processGroups.size());
    return TRUE;
}

DWORD WINAPI fakeGetProcessId(HANDLE /*process*/)
{
    return PID;
}

HANDLE WINAPI fakeOpenThread(DWORD /*access*/, BOOL /*inherit*/, DWORD tid)
{
    return fake().threadAffinities.contains(tid) ? newHandle() : nullptr;
}

DWORD WINAPI fakeGetProcessIdOfThread(HANDLE /*thread*/)
{
    return fake().threadOwnerPid;
}

/// The fakes read threads in snapshot order, so the next unread thread's affinity is reported.
BOOL WINAPI fakeGetThreadGroupAffinity(HANDLE /*thread*/, PGROUP_AFFINITY affinity)
{
    if (fake().threadAffinities.empty())
    {
        return FALSE;
    }
    auto it = fake().threadAffinities.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(fake().nextThread++ % fake().threadAffinities.size()));
    *affinity = it->second;
    return TRUE;
}

// --- EStats ---------------------------------------------------------------------------------

template<typename RowT>
DWORD WINAPI fakeSetEStats(RowT* row, TCP_ESTATS_TYPE /*type*/, PUCHAR /*rw*/, ULONG /*version*/, ULONG /*size*/, ULONG /*offset*/)
{
    if (row->dwLocalPort == 0) // The constructor's zeroed dummy row
    {
        return fake().dummySetStatus;
    }
    ++fake().enableCalls;
    return NO_ERROR;
}

template<typename RowT>
DWORD WINAPI fakeGetEStats(RowT* row,
                           TCP_ESTATS_TYPE /*type*/,
                           PUCHAR rw,
                           ULONG /*rwVersion*/,
                           ULONG /*rwSize*/,
                           PUCHAR /*ros*/,
                           ULONG /*rosVersion*/,
                           ULONG /*rosSize*/,
                           PUCHAR rod,
                           ULONG /*rodVersion*/,
                           ULONG /*rodSize*/)
{
    if (fake().readStatus != NO_ERROR)
    {
        return fake().readStatus;
    }
    for (const auto* rows : {&fake().rows4, &fake().rows6})
    {
        for (const FakeTcpRow& candidate : *rows)
        {
            if (candidate.localPort == row->dwLocalPort)
            {
                TCP_ESTATS_DATA_ROD_v0 data{};
                data.DataBytesIn = candidate.bytesIn;
                data.DataBytesOut = candidate.bytesOut;
                std::memcpy(rod, &data, sizeof(data));
                TCP_ESTATS_DATA_RW_v0 state{};
                state.EnableCollection = TRUE;
                std::memcpy(rw, &state, sizeof(state));
                return NO_ERROR;
            }
        }
    }
    return ERROR_NOT_FOUND;
}

WindowsProcessProbeFunctions::TcpEStats fakeLoadTcpEStats(Windows::UniqueModule& /*ownedModule*/)
{
    ++fake().loadCalls;
    if (!fake().estatsExported)
    {
        return {};
    }
    WindowsProcessProbeFunctions::TcpEStats estats{
        .getPerTcpConnectionEStats = &fakeGetEStats<MIB_TCPROW>,
        .setPerTcpConnectionEStats = &fakeSetEStats<MIB_TCPROW>,
    };
    if (fake().ipv6Exported)
    {
        estats.getPerTcp6ConnectionEStats = &fakeGetEStats<MIB_TCP6ROW>;
        estats.setPerTcp6ConnectionEStats = &fakeSetEStats<MIB_TCP6ROW>;
    }
    return estats;
}

template<typename TableT, typename RowT> DWORD fillTable(PVOID table, PDWORD size, const std::vector<FakeTcpRow>& rows)
{
    const auto needed = static_cast<DWORD>(offsetof(TableT, table) + (std::max<std::size_t>(rows.size(), 1) * sizeof(RowT)));
    if (table == nullptr || *size < needed)
    {
        *size = needed;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (fake().tableFillsTooSmall > 0)
    {
        --fake().tableFillsTooSmall;
        *size = needed + 8192; // Connections opened since the size query
        return ERROR_INSUFFICIENT_BUFFER;
    }
    auto* out = static_cast<TableT*>(table);
    out->dwNumEntries = static_cast<DWORD>(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        RowT row{};
        row.dwState = rows[i].state;
        row.dwLocalPort = rows[i].localPort;
        row.dwOwningPid = rows[i].pid;
        std::memcpy(static_cast<std::byte*>(table) + offsetof(TableT, table) + (i * sizeof(RowT)), &row, sizeof(row));
    }
    return NO_ERROR;
}

DWORD WINAPI
fakeGetExtendedTcpTable(PVOID table, PDWORD size, BOOL /*order*/, ULONG family, TCP_TABLE_CLASS /*tableClass*/, ULONG /*reserved*/)
{
    if (fake().failTableSize)
    {
        return ERROR_ACCESS_DENIED;
    }
    if (family == AF_INET6)
    {
        ++fake().table6Calls;
        return fillTable<MIB_TCP6TABLE_OWNER_PID, MIB_TCP6ROW_OWNER_PID>(table, size, fake().rows6);
    }
    return fillTable<MIB_TCPTABLE_OWNER_PID, MIB_TCPROW_OWNER_PID>(table, size, fake().rows4);
}

WindowsProcessProbeFunctions fakeFunctions()
{
    return {
        .network = {.loadTcpEStats = &fakeLoadTcpEStats, .getExtendedTcpTable = &fakeGetExtendedTcpTable},
        .system =
            {
                .ntQuerySystemInformation = &fakeNtQuerySystemInformation,
                .getMaximumProcessorGroupCount = &fakeGetMaximumProcessorGroupCount,
                .getMaximumProcessorCount = &fakeGetMaximumProcessorCount,
                .getLogicalProcessorInformationEx = &fakeGetLogicalProcessorInformationEx,
                .windowsBuildNumber = &fakeWindowsBuildNumber,
            },
        .token =
            {
                .openProcessToken = &fakeOpenProcessToken,
                .getTokenInformation = &fakeGetTokenInformation,
                .lookupAccountSidW = &fakeLookupAccountSidW,
            },
        .process =
            {
                .openProcess = &fakeOpenProcess,
                .getPriorityClass = &fakeGetPriorityClass,
                .queryFullProcessImageNameW = &fakeQueryFullProcessImageNameW,
                .ntQueryInformationProcess = &fakeNtQueryInformationProcess,
                .getGuiResources = &fakeGetGuiResources,
                .getProcessAffinityMask = &fakeGetProcessAffinityMask,
                .getProcessGroupAffinity = &fakeGetProcessGroupAffinity,
                .getProcessId = &fakeGetProcessId,
                .openThread = &fakeOpenThread,
                .getProcessIdOfThread = &fakeGetProcessIdOfThread,
                .getThreadGroupAffinity = &fakeGetThreadGroupAffinity,
            },
    };
}

class WindowsProcessProbeSeamTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = FakeWin{};
    }

    /// One enumerate() of a fresh probe, returning the fake process's counters.
    static ProcessCounters sampleOne()
    {
        WindowsProcessProbe probe(fakeFunctions());
        return find(probe.enumerate());
    }

    static ProcessCounters find(const std::vector<ProcessCounters>& processes)
    {
        const auto it = std::ranges::find(processes, static_cast<std::int32_t>(PID), &ProcessCounters::pid);
        EXPECT_NE(it, processes.end());
        return (it != processes.end()) ? *it : ProcessCounters{};
    }

    /// Elevated, with EStats exported and two IPv4 connections of one process (one more listening)
    /// and one IPv6 connection of another.
    static void useWorkingEStats()
    {
        fake().elevated = true;
        fake().rows4 = {
            {.localPort = 1, .pid = 10, .bytesIn = 100, .bytesOut = 10},
            {.localPort = 2, .pid = 10, .bytesIn = 200, .bytesOut = 20},
            {.state = MIB_TCP_STATE_LISTEN, .localPort = 3, .pid = 10},
        };
        fake().rows6 = {{.localPort = 4, .pid = 20, .bytesIn = 400, .bytesOut = 40}, {.state = MIB_TCP_STATE_TIME_WAIT, .localPort = 5}};
    }
};

// --- Per-process details ----------------------------------------------------------------------

TEST_F(WindowsProcessProbeSeamTest, EveryDetailIsReadWhenEveryCallSucceeds)
{
    const ProcessCounters process = sampleOne();
    EXPECT_EQ(process.name, "app.exe");
    EXPECT_EQ(process.user, "alice");
    EXPECT_EQ(process.command, "app.exe --flag");
    EXPECT_EQ(process.status, "Efficiency Mode");
    EXPECT_EQ(process.gdiObjectCount, 7);
    EXPECT_EQ(process.processType, "App");
    EXPECT_EQ(process.priorityClass, PriorityClass::High);
    EXPECT_EQ(process.nice, priorityClassToNice(HIGH_PRIORITY_CLASS));
    EXPECT_EQ(process.cpuAffinity, CpuAffinity::fromMask(0b101));
    EXPECT_EQ(fake().openProcessCalls, 1);
}

TEST_F(WindowsProcessProbeSeamTest, AnUnopenableProcessKeepsItsNameAndNothingElse)
{
    fake().failOpenProcess = true;
    const ProcessCounters process = sampleOne();
    EXPECT_EQ(process.command, "[app.exe]");
    EXPECT_TRUE(process.user.empty());
    EXPECT_TRUE(process.processType.empty());
    EXPECT_FALSE(process.gdiObjectCount.has_value());
    EXPECT_TRUE(process.cpuAffinity.empty());
}

TEST_F(WindowsProcessProbeSeamTest, EachFailedOwnerStepLeavesOnlyTheUserMissing)
{
    for (bool FakeWin::* const failure : {&FakeWin::failOpenToken, &FakeWin::failTokenSize, &FakeWin::failTokenRead, &FakeWin::failLookup})
    {
        fake() = FakeWin{};
        fake().*failure = true;
        const ProcessCounters process = sampleOne();
        EXPECT_TRUE(process.user.empty());
        EXPECT_EQ(process.command, "app.exe --flag");
        EXPECT_EQ(process.processType, "App");
    }
}

TEST_F(WindowsProcessProbeSeamTest, AnUnreadableCommandLineFallsBackToTheImagePathThenTheName)
{
    fake().failCommandLine = true;
    EXPECT_EQ(sampleOne().command, "C:\\fake\\app.exe");

    fake().failImagePath = true;
    const ProcessCounters process = sampleOne();
    EXPECT_EQ(process.command, "[app.exe]");
    EXPECT_EQ(process.processType, "App"); // From its USER objects, which need no path
}

TEST_F(WindowsProcessProbeSeamTest, LongImagePathsAndCommandLinesGrowTheirBuffers)
{
    fake().imagePath = L"C:\\" + std::wstring(400, L'p') + L"\\app.exe";
    fake().commandLine = L"app.exe " + std::wstring(3000, L'x');
    fake().failCommandLine = false;
    const ProcessCounters process = sampleOne();
    EXPECT_EQ(process.command.size(), 3008U);

    fake().failCommandLine = true;
    EXPECT_EQ(sampleOne().command.size(), fake().imagePath.size());
}

TEST_F(WindowsProcessProbeSeamTest, StatusFlagsAndAFailedStatusRead)
{
    fake().statusFlags = 0x10; // PEBI_IS_FROZEN
    EXPECT_EQ(sampleOne().status, "Suspended");
    fake().statusFlags = 0;
    EXPECT_TRUE(sampleOne().status.empty());
    fake().failStatus = true;
    const ProcessCounters process = sampleOne();
    EXPECT_TRUE(process.status.empty());
    EXPECT_EQ(process.user, "alice");
}

TEST_F(WindowsProcessProbeSeamTest, NoNtQueryInformationProcessLeavesStatusAndCommandLineUnread)
{
    WindowsProcessProbeFunctions functions = fakeFunctions();
    functions.process.ntQueryInformationProcess = nullptr;
    WindowsProcessProbe probe(functions);
    const ProcessCounters process = find(probe.enumerate());
    EXPECT_TRUE(process.status.empty());
    EXPECT_EQ(process.command, "C:\\fake\\app.exe");
}

TEST_F(WindowsProcessProbeSeamTest, ARefusedGdiReadIsRetriedWithQueryInformation)
{
    fake().failGdiOnLimitedHandle = true;
    const ProcessCounters process = sampleOne();
    EXPECT_EQ(process.gdiObjectCount, 7);
    EXPECT_EQ(process.processType, "App");
    EXPECT_EQ(fake().openProcessCalls, 2);

    fake().failGdi = true;
    const ProcessCounters unreadable = sampleOne();
    EXPECT_FALSE(unreadable.gdiObjectCount.has_value());
    EXPECT_TRUE(unreadable.processType.empty()); // Unknown: it may own windows
}

TEST_F(WindowsProcessProbeSeamTest, ProcessTypeFromThePathWhenThereAreNoWindows)
{
    fake().userObjects = 0;
    EXPECT_EQ(sampleOne().processType, "Background Process");
    fake().imagePath = L"C:\\Windows\\System32\\svchost.exe";
    EXPECT_EQ(sampleOne().processType, "Windows Process");
}

TEST_F(WindowsProcessProbeSeamTest, AFailedPriorityReadIsNormal)
{
    fake().priorityClass = 0; // GetPriorityClass failed
    const ProcessCounters process = sampleOne();
    EXPECT_EQ(process.nice, priorityClassToNice(0));
    EXPECT_EQ(process.priorityClass, toPriorityClass(0));
    EXPECT_EQ(process.user, "alice");
}

TEST_F(WindowsProcessProbeSeamTest, AReusedPidIsReadAgainButTheSameProcessIsCached)
{
    WindowsProcessProbe probe(fakeFunctions());
    EXPECT_EQ(find(probe.enumerate()).user, "alice");

    fake().user = L"bob";
    EXPECT_EQ(find(probe.enumerate()).user, "alice"); // Same process: the heavy TTL holds
    EXPECT_EQ(fake().openProcessCalls, 1);

    fake().processes.front().createTime += 1; // The PID now belongs to a new process
    EXPECT_EQ(find(probe.enumerate()).user, "bob");
    EXPECT_EQ(fake().openProcessCalls, 2);
}

TEST_F(WindowsProcessProbeSeamTest, AFailedSnapshotEnumeratesNothing)
{
    WindowsProcessProbeFunctions functions = fakeFunctions();
    functions.system.ntQuerySystemInformation = nullptr;
    EXPECT_TRUE(WindowsProcessProbe(functions).enumerate().empty());

    fake().failSnapshot = true;
    EXPECT_TRUE(WindowsProcessProbe(fakeFunctions()).enumerate().empty());
}

// --- Processor-group affinity -----------------------------------------------------------------

TEST_F(WindowsProcessProbeSeamTest, AnUnreadableSingleGroupMaskIsEmpty)
{
    fake().failAffinityMask = true;
    EXPECT_TRUE(sampleOne().cpuAffinity.empty());
}

TEST_F(WindowsProcessProbeSeamTest, AProcessInTheSecondGroupIsNumberedAfterTheFirst)
{
    fake().groupMaxima = {4, 4};
    fake().activeMasks = {0xF, 0xF};
    fake().processGroups = {1};
    fake().processMask = 0b11;
    const CpuAffinity affinity = sampleOne().cpuAffinity;
    EXPECT_EQ(affinity.count(), 2U);
    EXPECT_TRUE(affinity.test(4));
    EXPECT_TRUE(affinity.test(5));
}

TEST_F(WindowsProcessProbeSeamTest, ThreadsAreAskedWhenTheProcessSpansGroups)
{
    fake().groupMaxima = {4, 4};
    fake().activeMasks = {0xF, 0xF};
    fake().processGroups = {0, 1};
    fake().processMask = 0; // Threads explicitly in several groups
    fake().processes.front().threadIds = {100, 101};
    fake().threadAffinities = {
        {100, GROUP_AFFINITY{.Mask = 0b1, .Group = 0, .Reserved = {}}},
        {101, GROUP_AFFINITY{.Mask = 0b10, .Group = 1, .Reserved = {}}},
    };
    const CpuAffinity affinity = sampleOne().cpuAffinity;
    EXPECT_EQ(affinity.count(), 2U);
    EXPECT_TRUE(affinity.test(0));
    EXPECT_TRUE(affinity.test(5));

    fake().threadOwnerPid = PID + 1; // A thread ID reused by another process
    EXPECT_TRUE(sampleOne().cpuAffinity.empty());
}

TEST_F(WindowsProcessProbeSeamTest, EveryFailedMultiGroupReadLeavesTheAffinityEmpty)
{
    const auto twoGroups = []
    {
        fake() = FakeWin{};
        fake().groupMaxima = {4, 4};
        fake().activeMasks = {0xF, 0xF};
        fake().processGroups = {1};
    };
    twoGroups();
    fake().failGroupAffinity = true;
    EXPECT_TRUE(sampleOne().cpuAffinity.empty());

    twoGroups();
    fake().groupAffinityAlwaysTooSmall = true;
    EXPECT_TRUE(sampleOne().cpuAffinity.empty());

    twoGroups();
    fake().failLogicalProcessorInfo = true;
    EXPECT_TRUE(sampleOne().cpuAffinity.empty());

    twoGroups();
    fake().groupMaxima = {4, 0}; // A group whose size can't be read
    EXPECT_TRUE(sampleOne().cpuAffinity.empty());

    twoGroups();
    fake().processes.front().threadIds = {100}; // Not readable: the fakes don't know it
    fake().processMask = 0;
    EXPECT_TRUE(sampleOne().cpuAffinity.empty());
}

// --- EStats per-connection network counters -----------------------------------------------------

TEST_F(WindowsProcessProbeSeamTest, TheRealLoaderResolvesEveryEStatsExport)
{
    // Resolving exports changes nothing; the probe only calls them when elevated.
    Windows::UniqueModule module;
    const WindowsProcessProbeFunctions::TcpEStats estats = WindowsProcessProbe::systemFunctions().network.loadTcpEStats(module);
    EXPECT_NE(estats.getPerTcpConnectionEStats, nullptr);
    EXPECT_NE(estats.setPerTcpConnectionEStats, nullptr);
    EXPECT_NE(estats.getPerTcp6ConnectionEStats, nullptr);
    EXPECT_NE(estats.setPerTcp6ConnectionEStats, nullptr);
}

TEST_F(WindowsProcessProbeSeamTest, NonElevatedNeverLoadsEStats)
{
    const WindowsProcessProbe probe(fakeFunctions());
    EXPECT_EQ(fake().loadCalls, 0);
    EXPECT_FALSE(probe.capabilities().hasNetworkCounters);
    EXPECT_TRUE(probe.capabilities().hasReducedPrivileges);
    EXPECT_EQ(probe.readSocketTraffic().sampleTimeNs, 0U);
}

TEST_F(WindowsProcessProbeSeamTest, MissingExportsOrARefusedProbeTurnCountersOff)
{
    fake().elevated = true;
    fake().estatsExported = false;
    EXPECT_FALSE(WindowsProcessProbe(fakeFunctions()).capabilities().hasNetworkCounters);

    fake().estatsExported = true;
    fake().dummySetStatus = ERROR_NOT_SUPPORTED;
    const ProcessCapabilities unsupported = WindowsProcessProbe(fakeFunctions()).capabilities();
    EXPECT_FALSE(unsupported.hasNetworkCounters);
    EXPECT_FALSE(unsupported.networkCountersBlocked);

    fake().dummySetStatus = ERROR_ACCESS_DENIED;
    const ProcessCapabilities denied = WindowsProcessProbe(fakeFunctions()).capabilities();
    EXPECT_FALSE(denied.hasNetworkCounters);
    EXPECT_TRUE(denied.networkCountersBlocked); // Elevated, so elevating can't help
}

TEST_F(WindowsProcessProbeSeamTest, EachEstablishedConnectionIsReadAndEnabledOnce)
{
    useWorkingEStats();
    const WindowsProcessProbe probe(fakeFunctions());
    ASSERT_TRUE(probe.capabilities().hasNetworkCounters);

    const SocketTrafficReading reading = probe.readSocketTraffic();
    EXPECT_NE(reading.sampleTimeNs, 0U);
    ASSERT_EQ(reading.sockets.size(), 3U); // The listening row is left out
    std::uint64_t pid10Received = 0;
    std::uint64_t pid20Sent = 0;
    for (const SocketTrafficSample& socket : reading.sockets)
    {
        EXPECT_TRUE(socket.readable);
        pid10Received += (socket.pid == 10) ? socket.bytesReceived : 0;
        pid20Sent += (socket.pid == 20) ? socket.bytesSent : 0;
    }
    EXPECT_EQ(pid10Received, 300U);
    EXPECT_EQ(pid20Sent, 40U);
    EXPECT_EQ(fake().enableCalls, 3);

    EXPECT_EQ(probe.readSocketTraffic().sockets.size(), 3U);
    EXPECT_EQ(fake().enableCalls, 3); // Already enabled (#1418)
    EXPECT_TRUE(probe.capabilities().hasNetworkCounters);
}

TEST_F(WindowsProcessProbeSeamTest, ATableThatGrewDuringTheReadIsReadAgain)
{
    useWorkingEStats();
    const WindowsProcessProbe probe(fakeFunctions());
    fake().tableFillsTooSmall = 1;
    EXPECT_EQ(probe.readSocketTraffic().sockets.size(), 3U);

    fake().tableFillsTooSmall = 3; // Still growing after every retry: the family is unreadable
    EXPECT_EQ(probe.readSocketTraffic().sampleTimeNs, 0U);
}

TEST_F(WindowsProcessProbeSeamTest, AnUnreadableTableGivesNoReadingButKeepsCountersOn)
{
    useWorkingEStats();
    const WindowsProcessProbe probe(fakeFunctions());
    fake().failTableSize = true;
    const SocketTrafficReading reading = probe.readSocketTraffic();
    EXPECT_EQ(reading.sampleTimeNs, 0U);
    EXPECT_TRUE(reading.sockets.empty());
    EXPECT_TRUE(probe.capabilities().hasNetworkCounters);
}

TEST_F(WindowsProcessProbeSeamTest, WithoutIpv6ExportsOnlyIpv4IsWalked)
{
    useWorkingEStats();
    fake().ipv6Exported = false;
    const WindowsProcessProbe probe(fakeFunctions());
    EXPECT_EQ(probe.readSocketTraffic().sockets.size(), 2U);
    EXPECT_EQ(fake().table6Calls, 0);
}

TEST_F(WindowsProcessProbeSeamTest, AccessDeniedOnRealConnectionsTurnsCountersOff)
{
    useWorkingEStats();
    const WindowsProcessProbe probe(fakeFunctions());
    fake().readStatus = ERROR_ACCESS_DENIED;
    EXPECT_EQ(probe.readSocketTraffic().sampleTimeNs, 0U);
    const ProcessCapabilities capabilities = probe.capabilities();
    EXPECT_FALSE(capabilities.hasNetworkCounters);
    EXPECT_TRUE(capabilities.networkCountersBlocked);
    EXPECT_EQ(probe.readSocketTraffic().sampleTimeNs, 0U);
}

TEST_F(WindowsProcessProbeSeamTest, OnlyAStreakOfNotFoundReadsTurnsCountersOff)
{
    useWorkingEStats();
    const WindowsProcessProbe probe(fakeFunctions());
    fake().readStatus = ERROR_NOT_FOUND;
    for (std::size_t sample = 1; sample < MAX_INCONCLUSIVE_ESTATS_SAMPLES; ++sample)
    {
        const SocketTrafficReading reading = probe.readSocketTraffic();
        EXPECT_NE(reading.sampleTimeNs, 0U);
        EXPECT_TRUE(std::ranges::none_of(reading.sockets, &SocketTrafficSample::readable));
        EXPECT_TRUE(probe.capabilities().hasNetworkCounters);
    }
    EXPECT_EQ(probe.readSocketTraffic().sampleTimeNs, 0U);
    const ProcessCapabilities capabilities = probe.capabilities();
    EXPECT_FALSE(capabilities.hasNetworkCounters);
    EXPECT_FALSE(capabilities.networkCountersBlocked); // Not a denial
}

TEST_F(WindowsProcessProbeSeamTest, ASaneSampleVerifiesEStatsForGood)
{
    useWorkingEStats();
    const WindowsProcessProbe probe(fakeFunctions());
    EXPECT_NE(probe.readSocketTraffic().sampleTimeNs, 0U);
    fake().readStatus = ERROR_ACCESS_DENIED; // After verification a denial no longer switches it off
    EXPECT_NE(probe.readSocketTraffic().sampleTimeNs, 0U);
    EXPECT_TRUE(probe.capabilities().hasNetworkCounters);
}

} // namespace
} // namespace Platform
// NOLINTEND(misc-include-cleaner,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr,cppcoreguidelines-pro-type-union-access,readability-non-const-parameter)
