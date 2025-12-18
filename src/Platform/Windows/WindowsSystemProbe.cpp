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
// clang-format on

#include <array>
#include <chrono>
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
            // NOLINTNEXTLINE(clang-diagnostic-cast-function-type-mismatch)
            fn = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
        }
        initialized = true;
    }
    return fn;
}

} // namespace

WindowsSystemProbe::WindowsSystemProbe() : m_NumCores(0)
{
    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    m_NumCores = static_cast<int>(sysInfo.dwNumberOfProcessors);

    // Get hostname
    std::array<char, MAX_COMPUTERNAME_LENGTH + 1> hostBuffer{};
    DWORD bufferSize = static_cast<DWORD>(hostBuffer.size());
    if (GetComputerNameA(hostBuffer.data(), &bufferSize) != 0)
    {
        m_Hostname = hostBuffer.data();
    }
    else
    {
        m_Hostname = "unknown";
    }

    // Get CPU model from registry
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, R"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        std::array<char, 256> cpuBuffer{};
        DWORD cpuBufferSize = static_cast<DWORD>(cpuBuffer.size());
        DWORD type = 0;
        if (RegQueryValueExA(hKey, "ProcessorNameString", nullptr, &type, reinterpret_cast<LPBYTE>(cpuBuffer.data()), &cpuBufferSize) ==
            ERROR_SUCCESS)
        {
            m_CpuModel = cpuBuffer.data();
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
        RegCloseKey(hKey);
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

    uint64_t idle = filetimeToTicks(ftIdle);
    uint64_t kernel = filetimeToTicks(ftKernel);
    uint64_t user = filetimeToTicks(ftUser);
    uint64_t system = kernel - idle; // Actual kernel time

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
    std::vector<ProcessorPerformanceInfo> perfInfo(static_cast<size_t>(m_NumCores));
    ULONG returnLength = 0;

    NTSTATUS status = ntQuery(SystemProcessorPerformanceInformation,
                              perfInfo.data(),
                              static_cast<ULONG>(perfInfo.size() * sizeof(ProcessorPerformanceInfo)),
                              &returnLength);

    if (status != 0) // STATUS_SUCCESS = 0
    {
        spdlog::error("NtQuerySystemInformation failed: 0x{:08X}", static_cast<unsigned>(status));
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
        uint64_t kernelTicks = static_cast<uint64_t>(info.KernelTime.QuadPart);
        uint64_t idleTicks = static_cast<uint64_t>(info.IdleTime.QuadPart);
        uint64_t userTicks = static_cast<uint64_t>(info.UserTime.QuadPart);

        core.idle = idleTicks;
        core.system = kernelTicks - idleTicks; // Actual kernel time
        core.user = userTicks;

        // DPC and interrupt time are included in kernel time, but we can expose them
        // as irq/softirq for more detail if desired
        core.irq = static_cast<uint64_t>(info.InterruptTime.QuadPart);
        // DpcTime is deferred procedure calls (similar to softirq on Linux)
        core.softirq = static_cast<uint64_t>(info.DpcTime.QuadPart);

        counters.cpuPerCore.push_back(core);
    }

    spdlog::trace("Read per-core CPU for {} cores", coresReturned);
}

void WindowsSystemProbe::readMemoryCounters(SystemCounters& counters) const
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

void WindowsSystemProbe::readUptime(SystemCounters& counters) const
{
    // GetTickCount64 returns milliseconds since system start
    uint64_t uptimeMs = GetTickCount64();
    counters.uptimeSeconds = uptimeMs / 1000;

    // Calculate boot timestamp
    auto now = std::chrono::system_clock::now();
    auto nowEpoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    counters.bootTimestamp = static_cast<uint64_t>(nowEpoch) - counters.uptimeSeconds;
}

void WindowsSystemProbe::readStaticInfo(SystemCounters& counters) const
{
    counters.hostname = m_Hostname;
    counters.cpuModel = m_CpuModel;
    counters.cpuCoreCount = m_NumCores;
}

void WindowsSystemProbe::readCpuFreq(SystemCounters& counters) const
{
    // Read CPU frequency from registry (in MHz)
    // This is the base frequency; current frequency requires more complex APIs
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, R"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD mhz = 0;
        DWORD dataSize = sizeof(mhz);
        DWORD type = 0;
        if (RegQueryValueExA(hKey, "~MHz", nullptr, &type, reinterpret_cast<LPBYTE>(&mhz), &dataSize) == ERROR_SUCCESS)
        {
            counters.cpuFreqMHz = static_cast<uint64_t>(mhz);
        }
        RegCloseKey(hKey);
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
        .hasIoWait = false,  // Windows doesn't expose iowait
        .hasSteal = false,   // Windows doesn't expose steal time
        .hasLoadAvg = false, // Windows doesn't have load average
        .hasCpuFreq = true,  // From registry ~MHz
    };
}

long WindowsSystemProbe::ticksPerSecond() const
{
    // Windows FILETIME uses 100-nanosecond intervals
    return 10'000'000L;
}

} // namespace Platform
