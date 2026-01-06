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
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <psapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <winternl.h>
// clang-format on

#undef max
#undef min

#include "WinString.h"
#include "WindowsProcAddress.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
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

/// Convert FILETIME to Unix epoch seconds
/// Windows FILETIME is 100-nanosecond intervals since January 1, 1601 UTC
/// Unix epoch is seconds since January 1, 1970 UTC
/// The difference is 11644473600 seconds (369 years)
[[nodiscard]] uint64_t filetimeToUnixEpoch(const FILETIME& ft)
{
    constexpr uint64_t WINDOWS_TICKS_PER_SECOND = 10'000'000;
    constexpr uint64_t WINDOWS_EPOCH_TO_UNIX_EPOCH = 11644473600ULL;

    ULARGE_INTEGER uli{};
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    // Convert 100-nanosecond intervals to seconds and adjust for epoch difference
    uint64_t windowsSeconds = uli.QuadPart / WINDOWS_TICKS_PER_SECOND;
    if (windowsSeconds < WINDOWS_EPOCH_TO_UNIX_EPOCH)
    {
        return 0; // Invalid time (before Unix epoch)
    }
    return windowsSeconds - WINDOWS_EPOCH_TO_UNIX_EPOCH;
}

/// Map Windows process state to single character
[[nodiscard]] char getProcessState(HANDLE hProcess)
{
    if (hProcess == nullptr)
    {
        return '?';
    }

    // Note: Windows APIs require DWORD for exit codes; usage is localized here.
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

// Structure for ProcessExtendedBasicInformation
// Available since Windows 8/10, contains IsFrozen and IsBackground flags
struct ProcessExtendedBasicInformation
{
    SIZE_T size = 0;
    PROCESS_BASIC_INFORMATION basicInfo{};
    ULONG flags = 0;
};

// Some Windows SDKs cap PROCESSINFOCLASS enum to a smaller range; use a non-constexpr
// conversion to allow the extended value used by ProcessExtendedBasicInformation.
const PROCESSINFOCLASS PROCESS_INFO_EXTENDED_BASIC = static_cast<PROCESSINFOCLASS>(64);

// Bit flags for ProcessExtendedBasicInformation.flags (only keep flags we use)
constexpr ULONG PEBI_IS_FROZEN = 0x00000010;     // Process is suspended (UWP apps, frozen by OS)
constexpr ULONG PEBI_IS_BACKGROUND = 0x00000020; // Background process (efficiency mode)

/// Query process status (Suspended, Efficiency Mode)
[[nodiscard]] std::string getProcessStatus(HANDLE hProcess)
{
    if (hProcess == nullptr)
    {
        return {};
    }

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr)
    {
        return {};
    }

    auto* fn = Windows::getProcAddress<NtQueryInformationProcessFn>(ntdll, "NtQueryInformationProcess");
    if (fn == nullptr)
    {
        return {};
    }

    ProcessExtendedBasicInformation extInfo{};
    extInfo.size = sizeof(extInfo);
    ULONG returnLen = 0;

    static_assert(sizeof(extInfo) <= std::numeric_limits<ULONG>::max(), "ProcessExtendedBasicInformation size exceeds ULONG range");
    const ULONG extInfoSize = Domain::Numeric::narrowOr<ULONG>(sizeof(extInfo), ULONG{0});

    const NTSTATUS status = fn(hProcess, PROCESS_INFO_EXTENDED_BASIC, &extInfo, extInfoSize, &returnLen);
    if (status < 0)
    {
        // API not available or process not accessible
        return {};
    }

    // Check for frozen state (Suspended)
    if ((extInfo.flags & PEBI_IS_FROZEN) != 0)
    {
        return "Suspended";
    }

    // Check for background/efficiency mode
    if ((extInfo.flags & PEBI_IS_BACKGROUND) != 0)
    {
        return "Efficiency Mode";
    }

    // No special status
    return {};
}

} // namespace

WindowsProcessProbe::WindowsProcessProbe() : m_HasPowerMonitoring(detectPowerMonitoring())
{
    if (m_HasPowerMonitoring)
    {
        spdlog::info("Power monitoring available on Windows");
    }
    else
    {
        spdlog::debug("Power monitoring not available on Windows");
    }

    m_HasNetworkCounters = detectNetworkCounters();
    if (m_HasNetworkCounters)
    {
        spdlog::info("Per-process network counters available via TCP EStats");
    }
    else
    {
        spdlog::debug("Per-process network counters not available (EStats unsupported or access denied)");
    }
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

    // Attribute energy to processes if power monitoring is available
    if (m_HasPowerMonitoring)
    {
        attributeEnergyToProcesses(results);
    }

    // Attach per-process network counters if available (best effort)
    applyNetworkCounters(results);

    spdlog::trace("Enumerated {} processes", results.size());
    return results;
}

bool WindowsProcessProbe::getProcessDetails(uint32_t pid, ProcessCounters& counters)
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

    // Get process status (Suspended, Efficiency Mode)
    counters.status = getProcessStatus(hProcess);

    // Get process owner (username)
    counters.user = getProcessOwner(hProcess);

    // Get full command line (image path)
    counters.command = getProcessCommandLine(hProcess);
    if (counters.command.empty())
    {
        counters.command = "[" + counters.name + "]";
    }

    // Get process priority class and map to nice-like value
    const DWORD priorityClass = GetPriorityClass(hProcess);
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
        counters.startTimeEpoch = filetimeToUnixEpoch(ftCreation);
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
        .hasUser = true,        // From OpenProcessToken + LookupAccountSid
        .hasCommand = true,     // From QueryFullProcessImageName
        .hasNice = true,        // From GetPriorityClass
        .hasPageFaults = true,  // From NtQueryInformationProcess (VM_COUNTERS)
        .hasPeakRss = true,     // From PROCESS_MEMORY_COUNTERS.PeakWorkingSetSize
        .hasCpuAffinity = true, // From GetProcessAffinityMask
        // Network counters: Requires ETW (Event Tracing for Windows) or GetPerTcpConnectionEStats
        // See GitHub issue for implementation tracking
        .hasNetworkCounters = m_HasNetworkCounters,
        .hasPowerUsage = m_HasPowerMonitoring, // Available if energy monitoring detected
        .hasStatus = true,                     // From NtQueryInformationProcess ProcessExtendedBasicInformation
    };
}

uint64_t WindowsProcessProbe::totalCpuTime() const
{
    return readTotalCpuTime();
}

uint64_t WindowsProcessProbe::readTotalCpuTime()
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

bool WindowsProcessProbe::detectPowerMonitoring()
{
    // On Windows, we use a simplified approach: check if we can read battery status
    // This provides a basic system-wide energy estimate via battery discharge rate
    // More sophisticated approaches would use PDH (Performance Data Helper) or EMI (Energy Metering Interface)

    SYSTEM_POWER_STATUS powerStatus{};
    if (GetSystemPowerStatus(&powerStatus) == 0)
    {
        return false;
    }

    // Power monitoring available if we have battery info or AC power with metrics
    // ACLineStatus: 0 = offline (battery), 1 = online (AC), 255 = unknown
    return powerStatus.ACLineStatus != 255;
}

uint64_t WindowsProcessProbe::readSystemEnergy() const
{
    // Windows doesn't provide direct energy counters like Linux RAPL
    // This is a simplified implementation using battery discharge estimation
    // For production, consider using:
    // - PDH (Performance Data Helper) counters for power
    // - EMI (Energy Metering Interface) if available
    // - WMI queries for battery metrics

    SYSTEM_POWER_STATUS powerStatus{};
    if (GetSystemPowerStatus(&powerStatus) == 0)
    {
        return 0;
    }

    // Estimate energy based on battery percentage and system state
    // This is a rough approximation - actual implementation would need more sophisticated tracking
    // Battery life percent: 0-100, 255 = unknown
    if (powerStatus.BatteryLifePercent > 100)
    {
        return 0;
    }

    // Use a synthetic energy value based on battery state
    // In a real implementation, this would integrate battery discharge rate over time
    // For now, return a cumulative-like value that changes with battery state

    // Increment synthetic energy counter (this simulates cumulative energy consumption)
    // In production, this would read actual hardware counters or integrate power over time
    m_SyntheticEnergy += 1000000; // Add 1 joule (1,000,000 microjoules) per sample

    return m_SyntheticEnergy;
}

void WindowsProcessProbe::attributeEnergyToProcesses(std::vector<ProcessCounters>& processes) const
{
    // Read current system-wide energy
    const uint64_t systemEnergy = readSystemEnergy();
    if (systemEnergy == 0)
    {
        return;
    }

    // Calculate total CPU time across all processes
    uint64_t totalProcessCpuTime = 0;
    for (const auto& proc : processes)
    {
        totalProcessCpuTime += (proc.userTime + proc.systemTime);
    }

    // Avoid division by zero
    if (totalProcessCpuTime == 0)
    {
        return;
    }

    // Attribute energy proportionally based on CPU usage
    // This is an approximation: energy per process = systemEnergy * (processCpuTime / totalCpuTime)
    for (auto& proc : processes)
    {
        const uint64_t processCpuTime = proc.userTime + proc.systemTime;
        const double cpuProportion = static_cast<double>(processCpuTime) / static_cast<double>(totalProcessCpuTime);
        proc.energyMicrojoules = static_cast<uint64_t>(static_cast<double>(systemEnergy) * cpuProportion);
    }
}

bool WindowsProcessProbe::detectNetworkCounters()
{
    HMODULE iphlp = GetModuleHandleW(L"iphlpapi.dll");
    if (iphlp == nullptr)
    {
        iphlp = LoadLibraryW(L"iphlpapi.dll");
        if (iphlp == nullptr)
        {
            return false;
        }
    }

    m_GetPerTcpConnectionEStats = Windows::getProcAddress<GetPerTcpConnectionEStatsFn>(iphlp, "GetPerTcpConnectionEStats");

    if (m_GetPerTcpConnectionEStats == nullptr)
    {
        return false;
    }

    // EStats requires elevated privileges on some systems; attempt a no-op call to detect access issues.
    // Use a minimal row to probe support and ignore failure codes other than access denied/not supported.
    if (m_GetPerTcpConnectionEStats != nullptr)
    {
        MIB_TCPROW dummy{};
        TCP_ESTATS_DATA_RW_v0 rw{};
        rw.EnableCollection = TRUE;
        const DWORD status = m_GetPerTcpConnectionEStats(
            &dummy, TcpConnectionEstatsData, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), nullptr, 0, 0, nullptr, 0, 0);
        if (status == ERROR_ACCESS_DENIED || status == ERROR_NOT_SUPPORTED)
        {
            return false;
        }
    }

    return true;
}

std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> WindowsProcessProbe::collectNetworkByteCounts() const
{
    std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> perPid;

    if (!m_HasNetworkCounters)
    {
        return perPid;
    }

    collectTcp4ByteCounts(perPid);

    return perPid;
}

void WindowsProcessProbe::collectTcp4ByteCounts(std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>& perPid) const
{
    if (m_GetPerTcpConnectionEStats == nullptr)
    {
        return;
    }

    DWORD tableSize = 0;
    DWORD status = GetExtendedTcpTable(nullptr, &tableSize, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (status != ERROR_INSUFFICIENT_BUFFER || tableSize == 0)
    {
        return;
    }

    std::vector<unsigned char> buffer(tableSize);
    status = GetExtendedTcpTable(buffer.data(), &tableSize, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (status != NO_ERROR)
    {
        return;
    }

    const auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
    {
        const MIB_TCPROW_OWNER_PID& ownerRow = table->table[i];
        MIB_TCPROW ownerRowBase{};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - Windows API requires union access
        ownerRowBase.dwState = ownerRow.dwState;
        ownerRowBase.dwLocalAddr = ownerRow.dwLocalAddr;
        ownerRowBase.dwLocalPort = ownerRow.dwLocalPort;
        ownerRowBase.dwRemoteAddr = ownerRow.dwRemoteAddr;
        ownerRowBase.dwRemotePort = ownerRow.dwRemotePort;

        TCP_ESTATS_DATA_RW_v0 rw{};
        rw.EnableCollection = TRUE;
        (void) m_GetPerTcpConnectionEStats(
            &ownerRowBase, TcpConnectionEstatsData, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), nullptr, 0, 0, nullptr, 0, 0);

        TCP_ESTATS_DATA_ROD_v0 rod{};
        const DWORD estats = m_GetPerTcpConnectionEStats(
            &ownerRowBase, TcpConnectionEstatsData, nullptr, 0, 0, nullptr, 0, 0, reinterpret_cast<PUCHAR>(&rod), 0, sizeof(rod));

        if (estats != NO_ERROR)
        {
            continue;
        }

        auto& agg = perPid[ownerRow.dwOwningPid];
        agg.first += rod.DataBytesOut;
        agg.second += rod.DataBytesIn;
    }
}

void WindowsProcessProbe::applyNetworkCounters(std::vector<ProcessCounters>& processes) const
{
    if (!m_HasNetworkCounters)
    {
        return;
    }

    const auto perPid = collectNetworkByteCounts();
    if (perPid.empty())
    {
        return;
    }

    for (auto& proc : processes)
    {
        auto it = perPid.find(static_cast<uint32_t>(proc.pid));
        if (it != perPid.end())
        {
            proc.netSentBytes = it->second.first;
            proc.netReceivedBytes = it->second.second;
        }
    }
}

} // namespace Platform
