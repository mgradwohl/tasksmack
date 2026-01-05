#include "WindowsSystemProbe.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <iphlpapi.h>
// clang-format on

#include "WinString.h"
#include "WindowsProcAddress.h"

#include <array>
#include <chrono>
#include <concepts>
#include <type_traits>
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
using NtQuerySystemInformationFn = NTSTATUS(WINAPI*)(ULONG SystemInformationClass,
                                                     PVOID SystemInformation,
                                                     ULONG SystemInformationLength,
                                                     PULONG ReturnLength);

// System information class for per-processor performance
constexpr ULONG SystemProcessorPerformanceInformation = 8;

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
        std::array<wchar_t, 256> cpuBuffer{};
        DWORD cpuBufferSize = sizeof(cpuBuffer);
        if (RegGetValueW(HKEY_LOCAL_MACHINE,
                         LR"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)",
                         L"ProcessorNameString",
                         RRF_RT_REG_SZ,
                         nullptr,
                         cpuBuffer.data(),
                         &cpuBufferSize) == ERROR_SUCCESS)
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

    // Allocate buffer for all processors
    std::vector<ProcessorPerformanceInfo> perfInfo(m_NumCores);
    ULONG returnLength = 0;

    NTSTATUS status = ntQuery(SystemProcessorPerformanceInformation,
                              perfInfo.data(),
                              static_cast<ULONG>(perfInfo.size() * sizeof(ProcessorPerformanceInfo)),
                              &returnLength);

    if (status != 0) // STATUS_SUCCESS = 0
    {
        // NTSTATUS is a signed integral type; formatting with {:X} prints the underlying bit pattern in hex.
        spdlog::error("NtQuerySystemInformation failed: 0x{:08X}", status);
        return;
    }

    // Calculate actual number of cores returned
    size_t coresReturned = returnLength / sizeof(ProcessorPerformanceInfo);
    counters.cpuPerCore.reserve(coresReturned);

    for (size_t i = 0; i < coresReturned; ++i)
    {
        const auto& info = perfInfo[i];

        CpuCounters core{};
        // KernelTime includes idle, so subtract to get actual kernel/system time
        const uint64_t kernelTicks = largeIntegerToTicks(info.KernelTime);
        const uint64_t idleTicks = largeIntegerToTicks(info.IdleTime);
        const uint64_t userTicks = largeIntegerToTicks(info.UserTime);

        core.idle = idleTicks;
        core.system = kernelTicks - idleTicks; // Actual kernel time
        core.user = userTicks;

        // DPC and interrupt time are included in kernel time, but we can expose them
        // as irq/softirq for more detail if desired
        core.irq = largeIntegerToTicks(info.InterruptTime);
        // DpcTime is deferred procedure calls (similar to softirq on Linux)
        core.softirq = largeIntegerToTicks(info.DpcTime);

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

    // Windows doesn't separate buffers/cached like Linux
    // Total - Available gives us "used" which includes cached
    // Leave buffersBytes and cachedBytes at 0

    // Page file (swap)
    counters.memory.swapTotalBytes = memStatus.ullTotalPageFile - memStatus.ullTotalPhys;
    counters.memory.swapFreeBytes = memStatus.ullAvailPageFile - memStatus.ullAvailPhys;

    // Clamp to 0 if physical is larger than page file
    if (memStatus.ullTotalPageFile < memStatus.ullTotalPhys)
    {
        counters.memory.swapTotalBytes = 0;
        counters.memory.swapFreeBytes = 0;
    }
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
        .hasNetworkCounters = true, // Via GetIfTable
    };
}

long WindowsSystemProbe::ticksPerSecond() const
{
    // Windows FILETIME uses 100-nanosecond intervals
    return 10'000'000L;
}

void WindowsSystemProbe::readNetworkCounters(SystemCounters& counters)
{
    // Use GetIfTable to enumerate network interfaces and read byte counters.
    // This is the older API that works across all Windows versions.
    // First call with null buffer to get required size.
    DWORD tableSize = 0;
    if (GetIfTable(nullptr, &tableSize, FALSE) != ERROR_INSUFFICIENT_BUFFER)
    {
        spdlog::warn("GetIfTable size query failed");
        return;
    }

    // Allocate buffer for the interface table
    std::vector<std::byte> buffer(tableSize);
    auto* table = reinterpret_cast<MIB_IFTABLE*>(buffer.data());

    if (GetIfTable(table, &tableSize, FALSE) != NO_ERROR)
    {
        spdlog::warn("GetIfTable failed");
        return;
    }

    uint64_t totalRxBytes = 0;
    uint64_t totalTxBytes = 0;

    for (DWORD i = 0; i < table->dwNumEntries; ++i)
    {
        const MIB_IFROW& row = table->table[i];

        // Filter interfaces:
        // - Skip loopback (internal traffic)
        // - Skip non-network interface types (Bluetooth, etc.)
        // - Include Ethernet, Wi-Fi, and virtual adapters (VPN, Docker, etc.)
        if (row.dwType == IF_TYPE_SOFTWARE_LOOPBACK)
        {
            continue;
        }

        // Only include network-type interfaces:
        // IF_TYPE_ETHERNET_CSMACD (6) - Ethernet
        // IF_TYPE_IEEE80211 (71) - Wi-Fi
        // IF_TYPE_TUNNEL (131) - VPN tunnels
        // IF_TYPE_PPP (23) - PPP connections
        // IF_TYPE_PROP_VIRTUAL (53) - Virtual adapters (Hyper-V, Docker, etc.)
        const bool isNetworkInterface =
            ((row.dwType == IF_TYPE_ETHERNET_CSMACD) || (row.dwType == IF_TYPE_IEEE80211) || (row.dwType == IF_TYPE_TUNNEL) ||
             (row.dwType == IF_TYPE_PPP) || (row.dwType == IF_TYPE_PROP_VIRTUAL));

        if (!isNetworkInterface)
        {
            continue;
        }

        const uint64_t rxBytes = row.dwInOctets;
        const uint64_t txBytes = row.dwOutOctets;

        totalRxBytes += rxBytes;
        totalTxBytes += txBytes;

        // Store per-interface data
        SystemCounters::InterfaceCounters ifaceCounters;
        // KNOWN LIMITATION: MIB_IFROW.bDescr contains ANSI strings in the system's default code page,
        // not UTF-8. Interface names with non-ASCII characters may display incorrectly (mojibake).
        // This will be resolved when we upgrade to GetIfTable2() which provides proper Unicode strings.
        // See issue #349 for the GetIfTable2 migration plan.
        // bDescr is a fixed-size char array, null-terminated
        ifaceCounters.name = std::string(reinterpret_cast<const char*>(row.bDescr), row.dwDescrLen);
        // Remove any trailing null characters using std::erase
        std::erase(ifaceCounters.name, '\0');
        ifaceCounters.displayName = ifaceCounters.name; // Use same name for display
        ifaceCounters.rxBytes = rxBytes;
        ifaceCounters.txBytes = txBytes;
        // dwOperStatus: MIB_IF_OPER_STATUS enum - IF_OPER_STATUS_OPERATIONAL (1) means up
        ifaceCounters.isUp = (row.dwOperStatus == IF_OPER_STATUS_OPERATIONAL);
        // dwSpeed is in bits per second, convert to Mbps
        ifaceCounters.linkSpeedMbps = row.dwSpeed / 1'000'000ULL;
        counters.networkInterfaces.push_back(std::move(ifaceCounters));
    }

    counters.netRxBytes = totalRxBytes;
    counters.netTxBytes = totalTxBytes;
}

} // namespace Platform
