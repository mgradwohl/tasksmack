#include "WindowsSystemProbe.h"

#include "Platform/SystemTypes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

// clang-format off
// Windows headers - version macros set via CMake compile definitions
// (WIN32_LEAN_AND_MEAN, NOMINMAX, _WIN32_WINNT, WINVER, NTDDI_VERSION)
#include <winsock2.h>    // Must come before windows.h
#include <ws2ipdef.h>    // Required for MIB_IF_ROW2/MIB_IF_TABLE2 definitions in netioapi.h
#include <windows.h>
#include <winternl.h>
#include <iphlpapi.h>    // Network interface APIs (includes netioapi.h)
// clang-format on

#undef max
#undef min

#include "WinString.h"
#include "WindowsProcAddress.h"
#include "WindowsSystemProbeMath.h"

#include <array>
#include <chrono>
#include <concepts>
#include <format>
#include <span>
#include <type_traits>
#include <vector>

#include <psapi.h> // GetPerformanceInfo (K32GetPerformanceInfo, in kernel32)

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

template<std::integral T> [[nodiscard]] constexpr auto toU64NonNegative(T value) noexcept -> uint64_t
{
    if constexpr (std::is_signed_v<T>)
    {
        if (value < 0)
        {
            return 0;
        }
    }

    return static_cast<uint64_t>(value);
}

[[nodiscard]] uint64_t largeIntegerToTicks(const LARGE_INTEGER& value)
{
    return toU64NonNegative(value.QuadPart);
}

// NtQuerySystemInformation function pointer type
using NtQuerySystemInformationFn = NTSTATUS(WINAPI*)(ULONG systemInformationClass,
                                                     PVOID systemInformation,
                                                     ULONG systemInformationLength,
                                                     PULONG returnLength);

// System information class for per-processor performance
constexpr ULONG SystemProcessorPerformanceInformation = 8;

// System information class for page-file sizes (SYSTEM_PAGEFILE_INFORMATION chain)
constexpr ULONG SystemPageFileInformationClass = 18;

// STATUS_INFO_LENGTH_MISMATCH, spelled out: ntstatus.h conflicts with windows.h's subset.
constexpr NTSTATUS STATUS_INFO_LENGTH_MISMATCH_VALUE = static_cast<NTSTATUS>(0xC0000004L);

// Per-processor performance information structure
// This matches the undocumented SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION
struct ProcessorPerformanceInfo
{
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime; // Includes idle time
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG InterruptCount;
};

/// Get NtQuerySystemInformation function from ntdll.dll (lazy init)
[[nodiscard]] NtQuerySystemInformationFn getNtQuerySystemInformation()
{
    static NtQuerySystemInformationFn fn = nullptr;
    static bool initialized = false;

    if (!initialized)
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll != nullptr)
        {
            fn = Windows::getProcAddress<NtQuerySystemInformationFn>(ntdll, "NtQuerySystemInformation");
        }
        initialized = true;
    }
    return fn;
}

} // namespace

WindowsSystemProbe::WindowsSystemProbe()
{
    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    m_NumCores = sysInfo.dwNumberOfProcessors;

    // Get hostname (UTF-8 via wide API)
    std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> hostBuffer{};
    // Note: Windows APIs require DWORD for buffer sizes; explicit usage is intentional.
    DWORD bufferSize = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(hostBuffer.data(), &bufferSize) != 0)
    {
        m_Hostname = WinString::wideToUtf8(hostBuffer.data());
    }
    else
    {
        m_Hostname = "unknown";
    }

    // Get CPU model from registry
    {
        constexpr auto* cpuKeyPath = LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)";
        constexpr auto* cpuValueName = L"ProcessorNameString";

        DWORD cpuBufferSize = 0;
        const LSTATUS sizeStatus =
            RegGetValueW(HKEY_LOCAL_MACHINE, cpuKeyPath, cpuValueName, RRF_RT_REG_SZ, nullptr, nullptr, &cpuBufferSize);
        std::vector<wchar_t> cpuBuffer(cpuBufferSize / sizeof(wchar_t));
        // Derive the byte size back from the actually-allocated buffer rather than reusing
        // the raw registry-reported cpuBufferSize, which may be one wchar_t larger than
        // cpuBuffer.size() * sizeof(wchar_t) due to the truncating division above.
        auto cpuBufferBytes = static_cast<DWORD>(cpuBuffer.size() * sizeof(wchar_t));
        if (sizeStatus == ERROR_SUCCESS && !cpuBuffer.empty() &&
            RegGetValueW(HKEY_LOCAL_MACHINE, cpuKeyPath, cpuValueName, RRF_RT_REG_SZ, nullptr, cpuBuffer.data(), &cpuBufferBytes) ==
                ERROR_SUCCESS)
        {
            m_CpuModel = WinString::wideToUtf8(cpuBuffer.data());

            // Trim leading/trailing whitespace
            while (!m_CpuModel.empty() && m_CpuModel[0] == ' ')
            {
                m_CpuModel.erase(0, 1);
            }
            while (!m_CpuModel.empty() && m_CpuModel.back() == ' ')
            {
                m_CpuModel.pop_back();
            }
        }
    }
    if (m_CpuModel.empty())
    {
        m_CpuModel = "Unknown CPU";
    }

    spdlog::debug("WindowsSystemProbe initialized with {} cores, host={}, cpu={}", m_NumCores, m_Hostname, m_CpuModel);
}

SystemCounters WindowsSystemProbe::read()
{
    SystemCounters counters{};

    readCpuCounters(counters);
    readMemoryCounters(counters);
    readUptime(counters);
    readStaticInfo(counters);
    readCpuFreq(counters);
    readNetworkCounters(counters);

    return counters;
}

void WindowsSystemProbe::readCpuCounters(SystemCounters& counters) const
{
    // First, get total CPU via GetSystemTimes (always works)
    FILETIME ftIdle{};
    FILETIME ftKernel{};
    FILETIME ftUser{};

    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser) == 0)
    {
        spdlog::error("GetSystemTimes failed: {}", GetLastError());
        return;
    }

    // GetSystemTimes returns:
    // - idle: time spent idle
    // - kernel: time spent in kernel mode (includes idle time)
    // - user: time spent in user mode
    //
    // To get actual kernel time: kernel - idle

    const uint64_t idle = filetimeToTicks(ftIdle);
    const uint64_t kernel = filetimeToTicks(ftKernel);
    const uint64_t user = filetimeToTicks(ftUser);
    const uint64_t system = kernel - idle; // Actual kernel time

    counters.cpuTotal.idle = idle;
    counters.cpuTotal.system = system;
    counters.cpuTotal.user = user;

    // Now get per-core CPU via NtQuerySystemInformation
    readPerCoreCpuCounters(counters);
}

void WindowsSystemProbe::readPerCoreCpuCounters(SystemCounters& counters) const
{
    auto ntQuery = getNtQuerySystemInformation();
    if (ntQuery == nullptr)
    {
        spdlog::warn("NtQuerySystemInformation not available, per-core CPU disabled");
        return;
    }

    // winternl.h does not declare a named STATUS_INFO_LENGTH_MISMATCH constant.
    constexpr NTSTATUS STATUS_INFO_LENGTH_MISMATCH_NT = static_cast<NTSTATUS>(0xC0000004L);

    // Allocate buffer for all processors. m_NumCores (GetSystemInfo().dwNumberOfProcessors)
    // is capped at 64 on systems with multiple processor groups (>64 logical processors,
    // e.g. Threadripper/EPYC-class machines); grow and retry on a length mismatch instead
    // of silently reporting no per-core data on those machines, mirroring the retry loop
    // already used for the SystemProcessInformation query in WindowsProcessProbe.
    std::vector<ProcessorPerformanceInfo> perfInfo(m_NumCores);
    ULONG returnLength = 0;
    NTSTATUS status = 0;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        returnLength = 0;
        status = ntQuery(SystemProcessorPerformanceInformation,
                         perfInfo.data(),
                         static_cast<ULONG>(perfInfo.size() * sizeof(ProcessorPerformanceInfo)),
                         &returnLength);
        if (status != STATUS_INFO_LENGTH_MISMATCH_NT)
        {
            break;
        }
        // Grow with headroom: the processor count can change between calls.
        const std::size_t neededEntries = (static_cast<std::size_t>(returnLength) / sizeof(ProcessorPerformanceInfo)) + 8;
        perfInfo.assign(neededEntries, ProcessorPerformanceInfo{});
    }

    if (status != 0) // STATUS_SUCCESS = 0
    {
        // NTSTATUS is a signed integral type; formatting with {:X} prints the underlying bit pattern in hex.
        spdlog::error("NtQuerySystemInformation failed: 0x{:08X}", status);
        return;
    }

    // Calculate actual number of cores returned; clamp to the buffer size in case the
    // kernel ever reports a length longer than what was actually allocated.
    size_t coresReturned = std::min(returnLength / sizeof(ProcessorPerformanceInfo), perfInfo.size());
    counters.cpuPerCore.reserve(coresReturned);

    for (size_t i = 0; i < coresReturned; ++i)
    {
        const auto& info = perfInfo[i];

        // KernelTime includes idle, and DPC/interrupt time are inside kernel time; processorTimes()
        // splits them so CpuCounters::active() counts each tick once (#1032). DpcTime is the
        // closest Windows analogue of Linux softirq.
        CpuCounters core = processorTimes(largeIntegerToTicks(info.KernelTime),
                                          largeIntegerToTicks(info.IdleTime),
                                          largeIntegerToTicks(info.UserTime),
                                          largeIntegerToTicks(info.DpcTime),
                                          largeIntegerToTicks(info.InterruptTime));
        // Windows enumerates processors contiguously, so the index is the core's identity (#1229).
        core.coreId = i;
        counters.cpuPerCore.push_back(core);
    }

    spdlog::trace("Read per-core CPU for {} cores", coresReturned);
}

void WindowsSystemProbe::readMemoryCounters(SystemCounters& counters)
{
    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);

    if (GlobalMemoryStatusEx(&memStatus) == 0)
    {
        spdlog::error("GlobalMemoryStatusEx failed: {}", GetLastError());
        return;
    }

    counters.memory.totalBytes = memStatus.ullTotalPhys;
    counters.memory.freeBytes = memStatus.ullAvailPhys;
    counters.memory.availableBytes = memStatus.ullAvailPhys;
    counters.memory.hasAvailableBytes = true;

    // Cached: the system cache, i.e. the standby list plus the system working set (#1027). That is
    // the memory Windows holds as file cache and gives back on demand, the closest analogue of
    // Linux's page cache, and what Task Manager calls "Cached". It is already inside "available",
    // so Used (total - available) is unchanged. GetPerformanceInfo needs no privilege, unlike the
    // per-list breakdown (SystemMemoryListInformation).
    PERFORMANCE_INFORMATION perfInfo{};
    perfInfo.cb = sizeof(perfInfo);
    if (GetPerformanceInfo(&perfInfo, sizeof(perfInfo)) != 0)
    {
        counters.memory.cachedBytes = static_cast<uint64_t>(perfInfo.SystemCache) * static_cast<uint64_t>(perfInfo.PageSize);
    }

    // Swap: the page files' own sizes (#1026). The commit figures in MEMORYSTATUSEX describe RAM
    // plus page file, and the "free page file" derived from them underflowed whenever the
    // remaining commit was below the available RAM, which pinned swap at ~100 %.
    const SwapBytes swap = readSwap();
    counters.memory.swapTotalBytes = swap.totalBytes;
    counters.memory.swapFreeBytes = swap.freeBytes;
}

SwapBytes WindowsSystemProbe::readSwap()
{
    const auto ntQuerySystemInformation = getNtQuerySystemInformation();
    if (ntQuerySystemInformation == nullptr)
    {
        return {};
    }

    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);

    // One entry is 32 bytes plus its file name; 4 KiB holds dozens of page files. Grow once if the
    // OS says it needs more.
    std::vector<std::byte> buffer(4096);
    ULONG returnLength = 0;
    NTSTATUS status =
        ntQuerySystemInformation(SystemPageFileInformationClass, buffer.data(), static_cast<ULONG>(buffer.size()), &returnLength);
    if (status == STATUS_INFO_LENGTH_MISMATCH_VALUE && returnLength > buffer.size())
    {
        buffer.resize(returnLength);
        status = ntQuerySystemInformation(SystemPageFileInformationClass, buffer.data(), static_cast<ULONG>(buffer.size()), &returnLength);
    }
    if (status < 0)
    {
        spdlog::debug("NtQuerySystemInformation(SystemPageFileInformation) failed: 0x{:x}", static_cast<unsigned long>(status));
        return {};
    }

    const std::size_t used = std::min<std::size_t>(returnLength, buffer.size());
    const auto totals = sumPageFiles(std::span<const std::byte>(buffer.data(), used));
    // No page file at all is a valid answer: swap is then zero, as it should read.
    return totals.has_value() ? swapFromPageFiles(*totals, sysInfo.dwPageSize) : SwapBytes{};
}

void WindowsSystemProbe::readUptime(SystemCounters& counters)
{
    // GetTickCount64 returns milliseconds since system start
    const uint64_t uptimeMs = GetTickCount64();
    counters.uptimeSeconds = uptimeMs / 1000;

    // Calculate boot timestamp
    auto now = std::chrono::system_clock::now();
    auto nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    const uint64_t nowEpochSeconds = toU64NonNegative(nowEpoch);
    counters.bootTimestamp = (nowEpochSeconds > counters.uptimeSeconds) ? (nowEpochSeconds - counters.uptimeSeconds) : 0ULL;
}

void WindowsSystemProbe::readStaticInfo(SystemCounters& counters) const
{
    counters.hostname = m_Hostname;
    counters.cpuModel = m_CpuModel;
    counters.cpuCoreCount = m_NumCores;
}

void WindowsSystemProbe::readCpuFreq(SystemCounters& counters)
{
    // Read CPU frequency from registry (in MHz)
    // This is the base frequency; current frequency requires more complex APIs
    {
        DWORD mhz = 0;
        DWORD dataSize = sizeof(mhz);
        if (RegGetValueW(HKEY_LOCAL_MACHINE,
                         LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)",
                         L"~MHz",
                         RRF_RT_REG_DWORD,
                         nullptr,
                         &mhz,
                         &dataSize) == ERROR_SUCCESS)
        {
            counters.cpuFreqMHz = toU64NonNegative(mhz);
        }
    }
    // Load average is not available on Windows (leave at 0)
}

SystemCapabilities WindowsSystemProbe::capabilities() const
{
    return SystemCapabilities{
        .hasPerCoreCpu = true, // Via NtQuerySystemInformation
        .hasMemoryAvailable = true,
        .hasSwap = true,
        .hasUptime = true,
        .hasIoWait = false,         // Windows doesn't expose iowait
        .hasSteal = false,          // Windows doesn't expose steal time
        .hasLoadAvg = false,        // Windows doesn't have load average
        .hasCpuFreq = true,         // From registry ~MHz
        .hasNetworkCounters = true, // Via GetIfTable2 (64-bit counters, Unicode names)
    };
}

long WindowsSystemProbe::ticksPerSecond() const
{
    // Windows FILETIME uses 100-nanosecond intervals
    return 10'000'000L;
}

void WindowsSystemProbe::readNetworkCounters(SystemCounters& counters)
{
    // Use GetIfTable2 for 64-bit counters and proper Unicode interface names.
    // GetIfTable2 allocates the buffer internally; we must free it with FreeMibTable.
    // Available since Windows Vista/Server 2008.
    MIB_IF_TABLE2* table = nullptr;
    const DWORD status = GetIfTable2(&table);
    if (status != NO_ERROR || table == nullptr)
    {
        spdlog::warn("GetIfTable2 failed: {}", status);
        return;
    }

    uint64_t totalRxBytes = 0;
    uint64_t totalTxBytes = 0;

    for (ULONG i = 0; i < table->NumEntries; ++i)
    {
        const MIB_IF_ROW2& row = table->Table[i];

        // Loopback, non-network types and NDIS filter-module rows are not interfaces of their own
        // (#1030); see isCountedNetworkRow().
        if (!isCountedNetworkRow(row.Type, row.InterfaceAndOperStatusFlags.FilterInterface != 0))
        {
            continue;
        }
        // 64-bit byte counters - no more 32-bit overflow issues
        const uint64_t rxBytes = row.InOctets;
        const uint64_t txBytes = row.OutOctets;

        totalRxBytes += rxBytes;
        totalTxBytes += txBytes;

        // Store per-interface data
        SystemCounters::InterfaceCounters ifaceCounters;

        // MIB_IF_ROW2 provides proper Unicode strings:
        // - Alias: friendly name (e.g., "Wi-Fi", "Ethernet")
        // - Description: full adapter description (e.g., "Intel(R) Wi-Fi 6 AX201 160MHz")
        // Fallback chain for name: Alias -> Description -> Interface index
        const std::string alias = WinString::wideToUtf8(row.Alias);
        const std::string description = WinString::wideToUtf8(row.Description);

        if (!alias.empty())
        {
            ifaceCounters.name = alias;
        }
        else if (!description.empty())
        {
            ifaceCounters.name = description;
        }
        else
        {
            ifaceCounters.name = std::format("Interface {}", row.InterfaceIndex);
        }

        // Display name: prefer Description, fall back to name
        ifaceCounters.displayName = description.empty() ? ifaceCounters.name : description;

        ifaceCounters.rxBytes = rxBytes;
        ifaceCounters.txBytes = txBytes;

        // IF_OPER_STATUS enum - IfOperStatusUp (1) means interface is operational
        ifaceCounters.isUp = (row.OperStatus == IfOperStatusUp);

        // 64-bit link speeds in bits/sec - convert to Mbps
        // Use transmit speed (receive speed may differ on asymmetric links)
        // Windows uses 0 or ULONG64_MAX to indicate unknown speed
        constexpr uint64_t WINDOWS_UNKNOWN_LINK_SPEED = std::numeric_limits<uint64_t>::max();
        if (row.TransmitLinkSpeed == 0 || row.TransmitLinkSpeed == WINDOWS_UNKNOWN_LINK_SPEED)
        {
            ifaceCounters.linkSpeedMbps = 0;
        }
        else
        {
            ifaceCounters.linkSpeedMbps = row.TransmitLinkSpeed / 1'000'000ULL;
        }

        counters.networkInterfaces.push_back(std::move(ifaceCounters));
    }

    // Free the table allocated by GetIfTable2
    FreeMibTable(table);

    counters.netRxBytes = totalRxBytes;
    counters.netTxBytes = totalTxBytes;
}

} // namespace Platform
