#include "WindowsProcessProbe.h"

#include "Domain/Numeric.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <winternl.h>
// clang-format on

#include "WinString.h"
#include "WindowsProcAddress.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{

namespace
{

/// Convert FILETIME to 100-nanosecond intervals (ticks)
[[nodiscard]] uint64_t filetimeToTicks(const FILETIME& ft)
{
    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

/// Map Windows process state to single character
[[nodiscard]] char getProcessState(HANDLE hProcess)
{
    if (hProcess == nullptr)
    {
        return '?';
    }

    DWORD exitCode = 0;
    if (GetExitCodeProcess(hProcess, &exitCode) != 0)
    {
        if (exitCode == STILL_ACTIVE)
        {
            return 'R'; // Running
        }
        return 'Z'; // Zombie/terminated
    }
    return '?';
}

/// Get the username (owner) of a process
[[nodiscard]] std::string getProcessOwner(HANDLE hProcess)
{
    if (hProcess == nullptr)
    {
        return {};
    }

    HANDLE hToken = nullptr;
    if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken) == 0)
    {
        return {};
    }

    // Get token user size
    DWORD tokenInfoLen = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &tokenInfoLen);
    if (tokenInfoLen == 0)
    {
        CloseHandle(hToken);
        return {};
    }

    // Allocate buffer and get token user
    std::vector<BYTE> tokenInfo(tokenInfoLen);
    if (GetTokenInformation(hToken, TokenUser, tokenInfo.data(), tokenInfoLen, &tokenInfoLen) == 0)
    {
        CloseHandle(hToken);
        return {};
    }

    // Look up the user name from the SID
    if (tokenInfo.size() < sizeof(TOKEN_USER))
    {
        CloseHandle(hToken);
        return {};
    }

    TOKEN_USER tokenUser{};
    std::memcpy(&tokenUser, tokenInfo.data(), sizeof(TOKEN_USER));
    std::array<WCHAR, 256> userName{};
    std::array<WCHAR, 256> domainName{};
    // Fallback to the actual array size constant (256) if conversion fails
    DWORD userNameLen = Domain::Numeric::narrowOr<DWORD>(userName.size(), DWORD{256});
    DWORD domainNameLen = Domain::Numeric::narrowOr<DWORD>(domainName.size(), DWORD{256});
    SID_NAME_USE sidType{};

    if (LookupAccountSidW(nullptr, tokenUser.User.Sid, userName.data(), &userNameLen, domainName.data(), &domainNameLen, &sidType) == 0)
    {
        CloseHandle(hToken);
        return {};
    }

    CloseHandle(hToken);
    return WinString::wideToUtf8(userName.data());
}

/// Get the full command line (image path) of a process
[[nodiscard]] std::string getProcessCommandLine(HANDLE hProcess)
{
    if (hProcess == nullptr)
    {
        return {};
    }

    std::array<WCHAR, MAX_PATH> path{};
    // MAX_PATH is 260 on Windows; use it as fallback if conversion somehow fails
    DWORD size = Domain::Numeric::narrowOr<DWORD>(path.size(), DWORD{MAX_PATH});

    if (QueryFullProcessImageNameW(hProcess, 0, path.data(), &size) != 0)
    {
        return WinString::wideToUtf8(path.data());
    }
    return {};
}

using NtQueryInformationProcessFn = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

// Some Windows SDK versions omit VM_COUNTERS and/or the ProcessVmCounters enum value from public headers.
// Define what we need locally for compatibility.
struct TaskSmackVmCounters
{
    SIZE_T peakVirtualSize = 0;
    SIZE_T virtualSize = 0;
    ULONG pageFaultCount = 0;
    SIZE_T peakWorkingSetSize = 0;
    SIZE_T workingSetSize = 0;
    SIZE_T quotaPeakPagedPoolUsage = 0;
    SIZE_T quotaPagedPoolUsage = 0;
    SIZE_T quotaPeakNonPagedPoolUsage = 0;
    SIZE_T quotaNonPagedPoolUsage = 0;
    SIZE_T pagefileUsage = 0;
    SIZE_T peakPagefileUsage = 0;
};

constexpr PROCESSINFOCLASS PROCESS_INFO_VM_COUNTERS = static_cast<PROCESSINFOCLASS>(3);

struct ProcessVmInfo
{
    std::uint64_t virtualSizeBytes = 0;
    std::uint64_t pageFaultCount = 0;
};

[[nodiscard]] auto queryProcessVmInfo(HANDLE hProcess) -> std::optional<ProcessVmInfo>
{
    if (hProcess == nullptr)
    {
        return std::nullopt;
    }

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return std::nullopt;
    }

    auto* fn = Windows::getProcAddress<NtQueryInformationProcessFn>(ntdll, "NtQueryInformationProcess");
    if (fn == nullptr)
    {
        return std::nullopt;
    }

    TaskSmackVmCounters vm{};
    ULONG returnLen = 0;
    // Verify at compile time that sizeof(vm) fits in ULONG
    static_assert(sizeof(vm) <= std::numeric_limits<ULONG>::max(), "TaskSmackVmCounters size exceeds ULONG range");
    // Since we've verified the size fits, the narrowOr will never use the fallback
    const ULONG vmSize = Domain::Numeric::narrowOr<ULONG>(sizeof(vm), ULONG{0});
    const NTSTATUS status = fn(hProcess, PROCESS_INFO_VM_COUNTERS, &vm, vmSize, &returnLen);
    if (status < 0)
    {
        return std::nullopt;
    }

    ProcessVmInfo info;
    // Fallback to 0 if virtual size exceeds uint64_t range (should never happen)
    info.virtualSizeBytes = Domain::Numeric::narrowOr<uint64_t>(vm.virtualSize, uint64_t{0});
    // Fallback to 0 if page fault count exceeds uint64_t range (should never happen)
    info.pageFaultCount = Domain::Numeric::narrowOr<uint64_t>(vm.pageFaultCount, uint64_t{0});
    return info;
}

} // namespace

WindowsProcessProbe::WindowsProcessProbe()
{
    spdlog::debug("WindowsProcessProbe initialized");
}

std::vector<ProcessCounters> WindowsProcessProbe::enumerate()
{
    std::vector<ProcessCounters> results;

    // Create snapshot of all processes
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
    {
        spdlog::error("CreateToolhelp32Snapshot failed: {}", GetLastError());
        return results;
    }

    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof(pe32);

    if (Process32FirstW(hSnapshot, &pe32) == 0)
    {
        CloseHandle(hSnapshot);
        return results;
    }

    for (BOOL hasEntry = TRUE; hasEntry != FALSE; hasEntry = Process32NextW(hSnapshot, &pe32))
    {
        ProcessCounters counters{};
        // Fallback to 0 for PID/parent PID if out of range (should never happen in practice)
        counters.pid = Domain::Numeric::narrowOr<std::int32_t>(pe32.th32ProcessID, std::int32_t{0});
        counters.parentPid = Domain::Numeric::narrowOr<std::int32_t>(pe32.th32ParentProcessID, std::int32_t{0});
        counters.name = WinString::wideToUtf8(pe32.szExeFile);
        // Fallback to 0 for thread count if out of range
        counters.threadCount = Domain::Numeric::narrowOr<std::int32_t>(pe32.cntThreads, std::int32_t{0});

        // Get detailed info (CPU times, memory) - may fail for protected processes
        // Ignore return value - we still want to include process even if details fail
        (void) getProcessDetails(pe32.th32ProcessID, counters);

        results.push_back(std::move(counters));
    }

    CloseHandle(hSnapshot);

    spdlog::trace("Enumerated {} processes", results.size());
    return results;
}

bool WindowsProcessProbe::getProcessDetails(uint32_t pid, ProcessCounters& counters) const
{
    // Open process with limited access - some system processes won't allow full access
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);

    if (hProcess == nullptr)
    {
        // Can't access this process - leave defaults
        counters.state = '?';
        return false;
    }

    // Get process state
    counters.state = getProcessState(hProcess);

    // Get process owner (username)
    counters.user = getProcessOwner(hProcess);

    // Get full command line (image path)
    counters.command = getProcessCommandLine(hProcess);
    if (counters.command.empty())
    {
        counters.command = "[" + counters.name + "]";
    }

    // Get process priority class and map to nice-like value
    DWORD priorityClass = GetPriorityClass(hProcess);
    switch (priorityClass)
    {
    case IDLE_PRIORITY_CLASS:
        counters.nice = 19;
        break;
    case BELOW_NORMAL_PRIORITY_CLASS:
        counters.nice = 10;
        break;
    case NORMAL_PRIORITY_CLASS:
        counters.nice = 0;
        break;
    case ABOVE_NORMAL_PRIORITY_CLASS:
        counters.nice = -5;
        break;
    case HIGH_PRIORITY_CLASS:
        counters.nice = -10;
        break;
    case REALTIME_PRIORITY_CLASS:
        counters.nice = -20;
        break;
    default:
        counters.nice = 0;
        break;
    }

    // Get CPU times
    FILETIME ftCreation{};
    FILETIME ftExit{};
    FILETIME ftKernel{};
    FILETIME ftUser{};

    if (GetProcessTimes(hProcess, &ftCreation, &ftExit, &ftKernel, &ftUser) != 0)
    {
        // Convert to ticks (100-nanosecond intervals)
        counters.userTime = filetimeToTicks(ftUser);
        counters.systemTime = filetimeToTicks(ftKernel);
        counters.startTimeTicks = filetimeToTicks(ftCreation);
    }

    // Get memory info
    struct ProcessMemoryCountersEx
    {
        PROCESS_MEMORY_COUNTERS base{};
        SIZE_T privateUsage{};
    };

    ProcessMemoryCountersEx pmc{};
    pmc.base.cb = sizeof(pmc);

    if (GetProcessMemoryInfo(hProcess, &pmc.base, sizeof(pmc)) != 0)
    {
        counters.rssBytes = pmc.base.WorkingSetSize;
        counters.peakRssBytes = pmc.base.PeakWorkingSetSize;

        if (auto vmInfo = queryProcessVmInfo(hProcess))
        {
            counters.virtualBytes = vmInfo->virtualSizeBytes;
            counters.pageFaultCount = vmInfo->pageFaultCount;
        }
        else if (pmc.base.PagefileUsage != 0)
        {
            // Fallback: commit charge (not virtual address space size, but avoids reporting RSS/Private bytes as VIRT).
            counters.virtualBytes = pmc.base.PagefileUsage;
        }
        else
        {
            // Last resort: private bytes.
            counters.virtualBytes = pmc.privateUsage;
        }
    }

    // Get I/O counters
    IO_COUNTERS ioCounters{};
    if (GetProcessIoCounters(hProcess, &ioCounters) != 0)
    {
        counters.readBytes = ioCounters.ReadTransferCount;
        counters.writeBytes = ioCounters.WriteTransferCount;
    }

    // Get CPU affinity mask
    DWORD_PTR processAffinityMask = 0;
    DWORD_PTR systemAffinityMask = 0;
    if (GetProcessAffinityMask(hProcess, &processAffinityMask, &systemAffinityMask) != 0)
    {
        // Convert DWORD_PTR to uint64_t (may truncate on 32-bit, but we support 64-bit only)
        counters.cpuAffinityMask = static_cast<std::uint64_t>(processAffinityMask);
    }
    else
    {
        counters.cpuAffinityMask = 0;
    }

    CloseHandle(hProcess);
    return true;
}

ProcessCapabilities WindowsProcessProbe::capabilities() const
{
    return ProcessCapabilities{
        .hasIoCounters = true,
        .hasThreadCount = true,
        .hasUserSystemTime = true,
        .hasStartTime = true,
        .hasUser = true,            // From OpenProcessToken + LookupAccountSid
        .hasCommand = true,         // From QueryFullProcessImageName
        .hasNice = true,            // From GetPriorityClass
        .hasPageFaults = true,      // From NtQueryInformationProcess (VM_COUNTERS)
        .hasPeakRss = true,         // From PROCESS_MEMORY_COUNTERS.PeakWorkingSetSize
        .hasCpuAffinity = true,     // From GetProcessAffinityMask
        // Network counters: Requires ETW (Event Tracing for Windows) or GetPerTcpConnectionEStats
        // See GitHub issue for implementation tracking
        .hasNetworkCounters = false
    };
}

uint64_t WindowsProcessProbe::totalCpuTime() const
{
    return readTotalCpuTime();
}

uint64_t WindowsProcessProbe::readTotalCpuTime() const
{
    FILETIME ftIdle{};
    FILETIME ftKernel{};
    FILETIME ftUser{};

    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser) == 0)
    {
        spdlog::error("GetSystemTimes failed: {}", GetLastError());
        return 0;
    }

    // Total = kernel + user (kernel includes idle time)
    return filetimeToTicks(ftKernel) + filetimeToTicks(ftUser);
}

long WindowsProcessProbe::ticksPerSecond() const
{
    // Windows FILETIME uses 100-nanosecond intervals
    // 10,000,000 ticks per second
    return 10'000'000L;
}

uint64_t WindowsProcessProbe::systemTotalMemory() const
{
    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus) != 0)
    {
        return memStatus.ullTotalPhys;
    }

    spdlog::error("GlobalMemoryStatusEx failed: {}", GetLastError());
    return 0;
}

} // namespace Platform
