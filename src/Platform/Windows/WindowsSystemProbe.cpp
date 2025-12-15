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
// clang-format on

#include <chrono>
#include <thread>

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

} // namespace

WindowsSystemProbe::WindowsSystemProbe()
    : m_NumCores(0)
{
    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    m_NumCores = static_cast<int>(sysInfo.dwNumberOfProcessors);
    spdlog::debug("WindowsSystemProbe initialized with {} cores", m_NumCores);
}

SystemCounters WindowsSystemProbe::read()
{
    SystemCounters counters{};

    readCpuCounters(counters);
    readMemoryCounters(counters);
    readUptime(counters);

    return counters;
}

void WindowsSystemProbe::readCpuCounters(SystemCounters& counters) const
{
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
    // Windows doesn't provide nice, iowait, irq, softirq, steal, guest
    // Leave them at 0

    // Per-core CPU times are not easily available without PDH or WMI
    // Leave cpuPerCore empty for now
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
    auto nowEpoch =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    counters.bootTimestamp = static_cast<uint64_t>(nowEpoch) - counters.uptimeSeconds;
}

SystemCapabilities WindowsSystemProbe::capabilities() const
{
    return SystemCapabilities{
        .hasPerCoreCpu = false, // Would need PDH/WMI for per-core
        .hasMemoryAvailable = true,
        .hasSwap = true,
        .hasUptime = true,
        .hasIoWait = false, // Windows doesn't expose iowait
        .hasSteal = false,  // Windows doesn't expose steal time
    };
}

long WindowsSystemProbe::ticksPerSecond() const
{
    // Windows FILETIME uses 100-nanosecond intervals
    return 10'000'000L;
}

} // namespace Platform
