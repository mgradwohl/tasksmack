/// @file test_WindowsProcessProbeSeams.cpp
/// @brief WindowsProcessProbe against fake Win32/NT calls (#1718): the EStats network counters,
/// processor-group affinity and per-process detail failures the real APIs can't be made to show.
/// Every fake is side-effect free: the only real calls are CreateEventW for handles the probe may
/// close, and the probe's own RAM query. No process is opened, changed or ended.

#include "Domain/SamplingConfig.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ProcessTypes.h"
#include "Platform/Windows/WindowsEtwNetCollector.h"
#include "Platform/Windows/WindowsEtwNetMath.h"
#include "Platform/Windows/WindowsHandles.h"
#include "Platform/Windows/WindowsProcessActionsMath.h"
#include "Platform/Windows/WindowsProcessMachineMath.h"
#include "Platform/Windows/WindowsProcessProbe.h"
#include "Platform/Windows/WindowsProcessProbeMath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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

/// A UDP collector whose delivery and counters the test sets (#1492). Outside the anonymous
/// namespace: it is made with std::make_shared.
class FakeUdpCollector final : public IUdpTrafficCollector // NOLINT(misc-use-internal-linkage) - see above
{
  public:
    UdpCollectorStartResult start() override
    {
        return UdpCollectorStartResult::Started;
    }
    void stop() override
    {}
    [[nodiscard]] bool isDelivering() const override
    {
        return delivering;
    }
    [[nodiscard]] std::vector<UdpProcessTraffic> sample() override
    {
        ++samples;
        return traffic;
    }

    bool delivering = false;
    std::vector<UdpProcessTraffic> traffic;
    int samples = 0;
};

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
    bool activeGroupsOverrun = false;  ///< Claims one more active group than the buffer holds
    bool nameOutsideSnapshot = false;  ///< Image names point outside the snapshot buffer
    std::uint32_t buildNumber = 19045; // Before threads span groups by default
    // Token
    bool elevated = false;
    bool failOpenToken = false;
    bool failElevation = false;
    bool tokenUserTooShort = false; ///< TokenUser reads fewer bytes than a TOKEN_USER
    bool failTokenSize = false;
    bool failTokenRead = false;
    bool failLookup = false;
    std::wstring user = L"alice";
    std::optional<DWORD> sidRid; ///< The token user SID's last sub-authority; by default one per user name
    int lookupCalls = 0;
    // Process
    bool failOpenProcess = false;
    bool failQueryInfoOpen = false; ///< Only the PROCESS_QUERY_INFORMATION open is refused
    int openProcessCalls = 0;
    HANDLE queryInfoHandle = nullptr; // The last PROCESS_QUERY_INFORMATION open
    DWORD priorityClass = HIGH_PRIORITY_CLASS;
    std::wstring imagePath = L"C:\\fake\\app.exe";
    bool failImagePath = false;
    std::wstring commandLine = L"app.exe --flag";
    bool failStatus = false;
    bool failCommandLine = false;
    bool commandLineOutsideBuffer = false; ///< The command line's characters aren't in the returned buffer
    ULONG statusFlags = 0;                 // PROCESS_EXTENDED_BASIC_INFORMATION::Flags
    bool failPowerThrottling = false;
    ULONG powerControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED; // EcoQoS on: Efficiency Mode
    ULONG powerStateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    std::vector<ULONG> infoClasses; // Every NtQueryInformationProcess class asked for
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
    // Architecture (#2394): IMAGE_FILE_MACHINE_* values
    std::uint16_t processMachine = MachineMath::MACHINE_AMD64;
    std::uint16_t nativeMachine = MachineMath::MACHINE_AMD64;
    int processMachineCalls = 0;
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
    // The detail caches' clock: still unless a test moves it, so no TTL runs out between two
    // enumerate() calls however slowly the test runs (#2054).
    std::chrono::steady_clock::time_point now;
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
        static std::wstring outside = L"elsewhere.exe"; // Not in the snapshot buffer
        info.ImageName.Buffer = fake().nameOutsideSnapshot ? outside.data() : reinterpret_cast<PWSTR>(out + at);
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
    buffer->Group.ActiveGroupCount = static_cast<WORD>(fake().activeMasks.size() + (fake().activeGroupsOverrun ? 1U : 0U));
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

std::chrono::steady_clock::time_point fakeSteadyNow()
{
    return fake().now;
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
        if (fake().failElevation)
        {
            return FALSE;
        }
        TOKEN_ELEVATION elevation{.TokenIsElevated = fake().elevated ? 1U : 0U};
        std::memcpy(info, &elevation, sizeof(elevation));
        return TRUE;
    }
    const DWORD needed = fake().tokenUserTooShort ? DWORD{sizeof(DWORD)} : DWORD{sizeof(TOKEN_USER) + 16};
    if (fake().failTokenSize)
    {
        *returned = 0;
        return FALSE;
    }
    if (info == nullptr || length < needed)
    {
        *returned = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (fake().failTokenRead)
    {
        return FALSE;
    }
    if (fake().tokenUserTooShort)
    {
        return TRUE; // Read, but too short to hold a TOKEN_USER
    }
    // S-1-5-21-<rid>: revision 1, two sub-authorities, the NT authority. A user name of its own has
    // a SID of its own unless a test sets the RID, as a renamed account keeps its SID.
    auto* const sid = static_cast<std::byte*>(info) + sizeof(TOKEN_USER);
    constexpr std::array<std::byte, 8> HEADER{
        std::byte{1}, std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{5}};
    const std::array<DWORD, 2> subAuthorities{21, fake().sidRid.value_or(static_cast<DWORD>(std::hash<std::wstring>{}(fake().user)))};
    std::memcpy(sid, HEADER.data(), HEADER.size());
    std::memcpy(sid + HEADER.size(), subAuthorities.data(), sizeof(subAuthorities));
    TOKEN_USER user{};
    user.User.Sid = sid;
    std::memcpy(info, &user, sizeof(user));
    return TRUE;
}

BOOL WINAPI fakeLookupAccountSidW(
    LPCWSTR /*system*/, PSID /*sid*/, LPWSTR name, LPDWORD nameLength, LPWSTR /*domain*/, LPDWORD /*domainLength*/, PSID_NAME_USE /*use*/)
{
    ++fake().lookupCalls;
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
    if (fake().failOpenProcess || (fake().failQueryInfoOpen && access == PROCESS_QUERY_INFORMATION))
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

/// PROCESS_EXTENDED_BASIC_INFORMATION's layout.
struct ExtendedBasicInformation
{
    SIZE_T size;
    PROCESS_BASIC_INFORMATION basicInfo;
    ULONG flags;
};
static_assert(sizeof(void*) != 8 || sizeof(ExtendedBasicInformation) == 64, "The 64-byte structure Windows fills");

/// NtQueryInformationProcess as Windows answers it (#2312): ProcessBasicInformation (class 0) fills
/// PROCESS_EXTENDED_BASIC_INFORMATION only for a buffer of exactly its size whose Size field says
/// so, and every class but class 0 and ProcessCommandLineInformation (60) refuses the buffers the
/// probe passes -- class 64, ProcessTelemetryIdInformation, among them, as the real one does.
LONG NTAPI fakeNtQueryInformationProcess(HANDLE /*process*/, ULONG infoClass, PVOID buffer, ULONG length, PULONG returned)
{
    constexpr ULONG BASIC = 0;
    constexpr ULONG COMMAND_LINE = 60;
    fake().infoClasses.push_back(infoClass);
    if (infoClass == BASIC)
    {
        if (fake().failStatus)
        {
            return STATUS_ACCESS_DENIED_NT;
        }
        ExtendedBasicInformation info{};
        if (length != sizeof(info) || static_cast<const ExtendedBasicInformation*>(buffer)->size != sizeof(info))
        {
            return STATUS_INFO_LENGTH_MISMATCH_NT;
        }
        info.size = sizeof(info);
        info.flags = fake().statusFlags;
        std::memcpy(buffer, &info, sizeof(info));
        *returned = sizeof(info);
        return 0;
    }
    if (infoClass != COMMAND_LINE)
    {
        constexpr ULONG TELEMETRY_BYTES = 422; // What class 64 asked for on Windows 11 build 26300
        *returned = TELEMETRY_BYTES;
        return STATUS_INFO_LENGTH_MISMATCH_NT;
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
    static std::wstring outside; // Not in the caller's buffer
    outside.assign(fake().commandLine.size(), L'o');
    text.Buffer = fake().commandLineOutsideBuffer ? outside.data() : reinterpret_cast<PWSTR>(out + sizeof(UNICODE_STRING));
    std::memcpy(out, &text, sizeof(text));
    std::memcpy(out + sizeof(UNICODE_STRING), fake().commandLine.data(), textBytes);
    return 0;
}

/// GetProcessInformation(ProcessPowerThrottling): refuses any other class, size or version.
BOOL WINAPI fakeGetProcessInformation(HANDLE /*process*/, PROCESS_INFORMATION_CLASS infoClass, LPVOID info, DWORD size)
{
    if (infoClass != ProcessPowerThrottling || size != sizeof(PROCESS_POWER_THROTTLING_STATE) ||
        static_cast<const PROCESS_POWER_THROTTLING_STATE*>(info)->Version != PROCESS_POWER_THROTTLING_CURRENT_VERSION)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (fake().failPowerThrottling)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    auto* state = static_cast<PROCESS_POWER_THROTTLING_STATE*>(info);
    state->ControlMask = fake().powerControlMask;
    state->StateMask = fake().powerStateMask;
    return TRUE;
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

std::uint16_t fakeProcessMachine(HANDLE /*process*/)
{
    ++fake().processMachineCalls;
    return fake().processMachine;
}

std::uint16_t fakeNativeMachine()
{
    return fake().nativeMachine;
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
                .steadyNow = &fakeSteadyNow,
                .nativeMachine = &fakeNativeMachine,
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
                .getProcessInformation = &fakeGetProcessInformation,
                .getGuiResources = &fakeGetGuiResources,
                .getProcessAffinityMask = &fakeGetProcessAffinityMask,
                .getProcessGroupAffinity = &fakeGetProcessGroupAffinity,
                .getProcessId = &fakeGetProcessId,
                .openThread = &fakeOpenThread,
                .getProcessIdOfThread = &fakeGetProcessIdOfThread,
                .getThreadGroupAffinity = &fakeGetThreadGroupAffinity,
                .processMachine = &fakeProcessMachine,
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
    for (bool FakeWin::* const failure :
         {&FakeWin::failOpenToken, &FakeWin::failTokenSize, &FakeWin::failTokenRead, &FakeWin::tokenUserTooShort, &FakeWin::failLookup})
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

TEST_F(WindowsProcessProbeSeamTest, AnEmptyOrMisplacedCommandLineFallsBackToTheImagePath)
{
    fake().commandLine.clear(); // Read, but with no characters
    EXPECT_EQ(sampleOne().command, "C:\\fake\\app.exe");

    fake().commandLine = L"app.exe --flag";
    fake().commandLineOutsideBuffer = true; // Its characters aren't where the header says
    EXPECT_EQ(sampleOne().command, "C:\\fake\\app.exe");
}

TEST_F(WindowsProcessProbeSeamTest, AnImageNameOutsideTheSnapshotIsNotRead)
{
    fake().nameOutsideSnapshot = true;
    const ProcessCounters process = sampleOne();
    EXPECT_TRUE(process.name.empty());
    EXPECT_EQ(process.command, "app.exe --flag"); // The details don't need the name
}

TEST_F(WindowsProcessProbeSeamTest, ACreateTimeBeforeTheUnixEpochHasNoStartTime)
{
    fake().processes.front().createTime = 10'000'000; // One second into 1601
    const ProcessCounters process = sampleOne();
    EXPECT_EQ(process.startTimeTicks, 10'000'000U);
    EXPECT_EQ(process.startTimeEpoch, 0U);
    EXPECT_EQ(process.user, "alice"); // Still a real create time: the details are read and cached
}

TEST_F(WindowsProcessProbeSeamTest, TheSystemProcessIsAWindowsProcessWhateverItOwns)
{
    fake().processes.push_back(FakeProcess{.pid = 4, .name = L"System", .threadIds = {}});
    WindowsProcessProbe probe(fakeFunctions());
    const std::vector<ProcessCounters> processes = probe.enumerate();
    const auto system = std::ranges::find(processes, 4, &ProcessCounters::pid);
    ASSERT_NE(system, processes.end());
    EXPECT_EQ(system->processType, "Windows Process"); // Its USER objects (2) don't make it an App
    EXPECT_EQ(find(processes).processType, "App");
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

TEST_F(WindowsProcessProbeSeamTest, StatusIsReadThroughProcessBasicInformationNotTheTelemetryClass)
{
    // #2312: the status was asked for with class 64, ProcessTelemetryIdInformation, which refuses
    // the extended-basic buffer, so a frozen process never read as Suspended.
    fake().powerControlMask = 0;
    fake().powerStateMask = 0;
    fake().statusFlags = EXTENDED_BASIC_IS_FROZEN;
    EXPECT_EQ(sampleOne().status, "Suspended");
    EXPECT_NE(std::ranges::find(fake().infoClasses, ULONG{0}), fake().infoClasses.end());
    EXPECT_EQ(std::ranges::find(fake().infoClasses, ULONG{64}), fake().infoClasses.end());
}

TEST_F(WindowsProcessProbeSeamTest, EfficiencyModeIsEcoQosNotTheBackgroundFlag)
{
    // #2312: IsBackground (0x20) was taken for Efficiency Mode; it is execution-speed throttling.
    fake().statusFlags = 0x20; // IsBackground
    fake().powerControlMask = 0;
    fake().powerStateMask = 0;
    EXPECT_TRUE(sampleOne().status.empty());

    fake().statusFlags = 0;
    fake().powerControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    fake().powerStateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    EXPECT_EQ(sampleOne().status, "Efficiency Mode");

    fake().powerStateMask = 0; // Taken control of, but switched off (HighQoS)
    EXPECT_TRUE(sampleOne().status.empty());
}

TEST_F(WindowsProcessProbeSeamTest, SuspendedWinsOverEfficiencyMode)
{
    fake().statusFlags = EXTENDED_BASIC_IS_FROZEN;
    EXPECT_EQ(sampleOne().status, "Suspended");
}

TEST_F(WindowsProcessProbeSeamTest, EachStatusReadStandsWithoutTheOther)
{
    fake().failStatus = true; // No extended basic information: power throttling still read
    EXPECT_EQ(sampleOne().status, "Efficiency Mode");

    fake().failStatus = false;
    fake().failPowerThrottling = true; // No power throttling: IsFrozen still read
    fake().statusFlags = EXTENDED_BASIC_IS_FROZEN;
    EXPECT_EQ(sampleOne().status, "Suspended");
}

TEST_F(WindowsProcessProbeSeamTest, AnUnreadableStatusKeepsTheLastOneRead)
{
    WindowsProcessProbe probe(fakeFunctions());
    EXPECT_EQ(find(probe.enumerate()).status, "Efficiency Mode");

    fake().failStatus = true;
    fake().failPowerThrottling = true;
    fake().now += std::chrono::hours(1); // Past every TTL, so the status is read again
    const ProcessCounters process = find(probe.enumerate());
    EXPECT_EQ(process.status, "Efficiency Mode"); // Unknown is not "nothing to show"
    EXPECT_EQ(process.user, "alice");

    EXPECT_TRUE(sampleOne().status.empty()); // A process never read has no status
}

TEST_F(WindowsProcessProbeSeamTest, NoNtQueryInformationProcessLeavesTheCommandLineAndFrozenStateUnread)
{
    WindowsProcessProbeFunctions functions = fakeFunctions();
    functions.process.ntQueryInformationProcess = nullptr;
    fake().statusFlags = EXTENDED_BASIC_IS_FROZEN; // Not read without it
    WindowsProcessProbe probe(functions);
    const ProcessCounters process = find(probe.enumerate());
    EXPECT_EQ(process.status, "Efficiency Mode"); // Power throttling needs no ntdll
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

    // The retry's own open refused: no count, and the type is unknown too.
    fake().failGdi = false;
    fake().failQueryInfoOpen = true;
    const ProcessCounters unopenable = sampleOne();
    EXPECT_FALSE(unopenable.gdiObjectCount.has_value());
    EXPECT_TRUE(unopenable.processType.empty());
    EXPECT_EQ(unopenable.user, "alice");
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

TEST_F(WindowsProcessProbeSeamTest, TheSameProcessIsReadAgainOnceItsTtlsRunOut)
{
    WindowsProcessProbe probe(fakeFunctions());
    EXPECT_EQ(find(probe.enumerate()).user, "alice");
    fake().user = L"bob";

    // An hour on is past the light and heavy TTLs whatever this machine's RAM tier makes them.
    fake().now += std::chrono::hours(1);
    EXPECT_EQ(find(probe.enumerate()).user, "bob");
    EXPECT_EQ(fake().openProcessCalls, 2);
}

// --- Account names (#2982) ---------------------------------------------------------------------

/// The default fake process, under another PID.
FakeProcess processWithPid(DWORD pid)
{
    FakeProcess process;
    process.pid = pid;
    return process;
}

TEST_F(WindowsProcessProbeSeamTest, ProcessesOfOneAccountAskForItsNameOnce)
{
    fake().processes = {FakeProcess{}, processWithPid(PID + 4), processWithPid(PID + 8)};
    WindowsProcessProbe probe(fakeFunctions());
    const auto processes = probe.enumerate();
    ASSERT_EQ(processes.size(), 3U);
    for (const ProcessCounters& process : processes)
    {
        EXPECT_EQ(process.user, "alice");
    }
    EXPECT_EQ(fake().lookupCalls, 1);

    // Each heavy pass reads the token again, but the name comes from the cache.
    fake().now += std::chrono::minutes(1);
    EXPECT_EQ(find(probe.enumerate()).user, "alice");
    EXPECT_EQ(fake().lookupCalls, 1);
}

TEST_F(WindowsProcessProbeSeamTest, AnotherAccountIsAskedForItsOwnName)
{
    WindowsProcessProbe probe(fakeFunctions());
    EXPECT_EQ(find(probe.enumerate()).user, "alice");

    fake().user = L"bob"; // A new process, owned by another account (another SID)
    fake().processes.push_back(processWithPid(PID + 4));
    const auto processes = probe.enumerate();
    const auto bob = std::ranges::find(processes, static_cast<std::int32_t>(PID + 4), &ProcessCounters::pid);
    ASSERT_NE(bob, processes.end());
    EXPECT_EQ(bob->user, "bob");
    EXPECT_EQ(find(processes).user, "alice"); // Still within its heavy TTL
    EXPECT_EQ(fake().lookupCalls, 2);
}

TEST_F(WindowsProcessProbeSeamTest, AFailedLookupIsNotCachedAndIsAskedAgain)
{
    fake().failLookup = true;
    WindowsProcessProbe probe(fakeFunctions());
    EXPECT_TRUE(find(probe.enumerate()).user.empty());
    EXPECT_EQ(fake().lookupCalls, 1);

    fake().failLookup = false;
    fake().now += std::chrono::minutes(1); // Past the heavy TTL, well within the name cache's
    EXPECT_EQ(find(probe.enumerate()).user, "alice");
    EXPECT_EQ(fake().lookupCalls, 2);
}

TEST_F(WindowsProcessProbeSeamTest, ARenamedAccountShowsItsNewNameOnceTheCachedNameExpires)
{
    fake().sidRid = 1001;
    WindowsProcessProbe probe(fakeFunctions());
    EXPECT_EQ(find(probe.enumerate()).user, "alice");

    fake().user = L"alicia"; // The same SID, renamed
    fake().now += std::chrono::minutes(1);
    EXPECT_EQ(find(probe.enumerate()).user, "alice");
    EXPECT_EQ(fake().lookupCalls, 1);

    fake().now += std::chrono::seconds(Domain::Sampling::ACCOUNT_NAME_CACHE_TTL_SECONDS);
    EXPECT_EQ(find(probe.enumerate()).user, "alicia");
    EXPECT_EQ(fake().lookupCalls, 2);
}

// --- Architecture (#2394) ------------------------------------------------------------------------

TEST_F(WindowsProcessProbeSeamTest, TheArchitectureIsReadOnceAProcessAndAReusedPidReadsItAgain)
{
    fake().processMachine = MachineMath::MACHINE_AMD64;
    fake().nativeMachine = MachineMath::MACHINE_ARM64;
    WindowsProcessProbe probe(fakeFunctions());
    EXPECT_EQ(find(probe.enumerate()).architecture, "x64 (emulated)");

    // Past every TTL: the other details are read again, the architecture never is.
    fake().processMachine = MachineMath::MACHINE_ARM64;
    fake().user = L"bob";
    fake().now += std::chrono::hours(1);
    const ProcessCounters again = find(probe.enumerate());
    EXPECT_EQ(again.user, "bob");
    EXPECT_EQ(again.architecture, "x64 (emulated)");
    EXPECT_EQ(fake().processMachineCalls, 1);

    fake().processes.front().createTime += 1; // The PID now belongs to a new process
    EXPECT_EQ(find(probe.enumerate()).architecture, "ARM64");
    EXPECT_EQ(fake().processMachineCalls, 2);
}

TEST_F(WindowsProcessProbeSeamTest, TheArchitectureIsComparedWithTheMachineWindowsRunsAs)
{
    const auto architecture = [](std::uint16_t process, std::uint16_t native)
    {
        fake().processMachine = process;
        fake().nativeMachine = native;
        return sampleOne().architecture;
    };
    EXPECT_EQ(architecture(MachineMath::MACHINE_AMD64, MachineMath::MACHINE_AMD64), "x64");
    EXPECT_EQ(architecture(MachineMath::MACHINE_I386, MachineMath::MACHINE_AMD64), "x86 (WOW64)");
    EXPECT_EQ(architecture(MachineMath::MACHINE_ARM64, MachineMath::MACHINE_ARM64), "ARM64");
    EXPECT_EQ(architecture(MachineMath::MACHINE_ARM64EC, MachineMath::MACHINE_ARM64), "ARM64EC");
    EXPECT_EQ(architecture(MachineMath::MACHINE_AMD64, MachineMath::MACHINE_ARM64), "x64 (emulated)");
    EXPECT_EQ(architecture(MachineMath::MACHINE_I386, MachineMath::MACHINE_ARM64), "x86 (emulated)");
    EXPECT_EQ(architecture(MachineMath::MACHINE_ARMNT, MachineMath::MACHINE_ARM64), "ARM32 (WOW64)");
    EXPECT_EQ(architecture(MachineMath::MACHINE_AMD64, MachineMath::MACHINE_UNKNOWN), "x64"); // Native unread
    EXPECT_TRUE(architecture(MachineMath::MACHINE_UNKNOWN, MachineMath::MACHINE_AMD64).empty());
}

TEST_F(WindowsProcessProbeSeamTest, AnUnopenableProcessHasNoArchitectureUntilItOpens)
{
    fake().failOpenProcess = true;
    WindowsProcessProbe probe(fakeFunctions());
    EXPECT_TRUE(find(probe.enumerate()).architecture.empty());
    EXPECT_EQ(fake().processMachineCalls, 0);

    fake().failOpenProcess = false;
    fake().now += std::chrono::hours(1); // The next handle opens with the details' TTL
    EXPECT_EQ(find(probe.enumerate()).architecture, "x64");
    EXPECT_EQ(fake().processMachineCalls, 1);
}

TEST_F(WindowsProcessProbeSeamTest, WithoutTheMachineReadsTheArchitectureIsUnreadOrNamedAlone)
{
    WindowsProcessProbeFunctions functions = fakeFunctions();
    functions.process.processMachine = nullptr;
    EXPECT_TRUE(find(WindowsProcessProbe(functions).enumerate()).architecture.empty());

    functions = fakeFunctions();
    functions.system.nativeMachine = nullptr;
    fake().processMachine = MachineMath::MACHINE_I386;
    EXPECT_EQ(find(WindowsProcessProbe(functions).enumerate()).architecture, "x86");
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
    fake().activeMasks = {0xF};
    fake().activeGroupsOverrun = true; // Two active groups claimed, one returned
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

TEST_F(WindowsProcessProbeSeamTest, AnUnreadableElevationCountsAsNotElevated)
{
    fake().elevated = true;
    fake().failElevation = true;
    const WindowsProcessProbe probe(fakeFunctions());
    EXPECT_EQ(fake().loadCalls, 0);
    EXPECT_FALSE(probe.capabilities().hasNetworkCounters);
    EXPECT_TRUE(probe.capabilities().hasReducedPrivileges);
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

// --- The opt-in UDP event trace (#1492) ------------------------------------------------------

TEST_F(WindowsProcessProbeSeamTest, TcpOnlyUntilTheUdpCollectorDelivers)
{
    useWorkingEStats();
    WindowsProcessProbe probe(fakeFunctions());
    EXPECT_FALSE(probe.capabilities().hasUdpNetworkCounters); // No collector: TCP only

    const auto udp = std::make_shared<FakeUdpCollector>();
    probe.setUdpTrafficCollector(udp);
    EXPECT_FALSE(probe.capabilities().hasUdpNetworkCounters); // Started, nothing delivered yet
    EXPECT_EQ(probe.readSocketTraffic().sockets.size(), 3U);

    udp->delivering = true;
    udp->traffic = {UdpProcessTraffic{.pid = 10, .bytesSent = 500, .bytesReceived = 600}};
    EXPECT_TRUE(probe.capabilities().hasUdpNetworkCounters);
    const SocketTrafficReading reading = probe.readSocketTraffic();
    ASSERT_EQ(reading.sockets.size(), 4U); // The three TCP connections, then process 10's UDP
    const SocketTrafficSample& udpSample = reading.sockets.back();
    EXPECT_EQ(udpSample.key, EtwNet::udpSocketKey(10));
    EXPECT_EQ(udpSample.pid, 10);
    EXPECT_EQ(udpSample.bytesSent, 500U);
    EXPECT_EQ(udpSample.bytesReceived, 600U);
    EXPECT_TRUE(udpSample.readable);
}

TEST_F(WindowsProcessProbeSeamTest, UdpIsReadOnlyForAReadingDomainFolds)
{
    useWorkingEStats();
    WindowsProcessProbe probe(fakeFunctions());
    const auto udp = std::make_shared<FakeUdpCollector>();
    udp->delivering = true;
    probe.setUdpTrafficCollector(udp);
    fake().failTableSize = true; // No complete TCP walk: no reading, and the cumulative UDP waits
    EXPECT_EQ(probe.readSocketTraffic().sampleTimeNs, 0U);
    EXPECT_EQ(udp->samples, 0);
}

TEST_F(WindowsProcessProbeSeamTest, WithoutTcpCountersTheUdpCollectorAddsNothing)
{
    // Not elevated: no per-process network counters at all, so no UDP either (the collector itself
    // never starts then; a delivering one changes nothing here).
    WindowsProcessProbe probe(fakeFunctions());
    const auto udp = std::make_shared<FakeUdpCollector>();
    udp->delivering = true;
    probe.setUdpTrafficCollector(udp);
    EXPECT_FALSE(probe.capabilities().hasUdpNetworkCounters);
    EXPECT_EQ(probe.readSocketTraffic().sampleTimeNs, 0U);
    EXPECT_EQ(udp->samples, 0);
}

TEST_F(WindowsProcessProbeSeamTest, ADeniedStartTraceLeavesTheCountersTcpOnly)
{
    // The real collector, its StartTrace refused: the column note keeps saying "TCP only".
    useWorkingEStats();
    WindowsEtwNetCollector::Functions etw{};
    etw.isElevated = []
    {
        return true;
    };
    etw.self = []
    {
        return EtwNet::SessionOwner{.pid = 1, .creationTime = 1};
    };
    etw.ownerLiveness = [](const EtwNet::SessionOwner&)
    {
        return EtwNet::OwnerLiveness::Unknown;
    };
    etw.queryAllTracesW = [](PEVENT_TRACE_PROPERTIES*, ULONG, PULONG count) -> ULONG
    {
        *count = 0;
        return ERROR_SUCCESS;
    };
    etw.startTraceW = [](ULONG64*, LPCWSTR, PEVENT_TRACE_PROPERTIES) -> ULONG
    {
        return ERROR_ACCESS_DENIED;
    };
    etw.controlTraceW = [](ULONG64, LPCWSTR, PEVENT_TRACE_PROPERTIES, ULONG) -> ULONG
    {
        return ERROR_SUCCESS;
    };
    etw.closeTrace = [](ULONG64) -> ULONG
    {
        return ERROR_SUCCESS;
    };
    const auto collector = std::make_shared<WindowsEtwNetCollector>(etw);
    EXPECT_EQ(collector->start(), UdpCollectorStartResult::Denied);

    WindowsProcessProbe probe(fakeFunctions());
    probe.setUdpTrafficCollector(collector);
    const ProcessCapabilities capabilities = probe.capabilities();
    EXPECT_TRUE(capabilities.hasNetworkCounters);
    EXPECT_FALSE(capabilities.hasUdpNetworkCounters);
    EXPECT_EQ(probe.readSocketTraffic().sockets.size(), 3U); // TCP only
}

} // namespace
} // namespace Platform
// NOLINTEND(misc-include-cleaner,cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr,cppcoreguidelines-pro-type-union-access,readability-non-const-parameter)
