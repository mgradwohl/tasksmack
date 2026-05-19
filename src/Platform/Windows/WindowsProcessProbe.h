#pragma once

#include "Platform/IProcessProbe.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // NOLINT(cppcoreguidelines-macro-usage) - Windows platform requirement
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

// Windows headers must be in correct order:
// winsock2.h must come before windows.h
// windows.h must come before iphlpapi.h
// clang-format off
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mstcpip.h>
// clang-format on

namespace Platform
{

/// Windows implementation of IProcessProbe.
/// Uses ToolHelp32 API and GetProcessTimes/GetProcessMemoryInfo.
class WindowsProcessProbe : public IProcessProbe
{
  public:
    WindowsProcessProbe();
    ~WindowsProcessProbe() override;

    WindowsProcessProbe(const WindowsProcessProbe&) = delete;
    WindowsProcessProbe& operator=(const WindowsProcessProbe&) = delete;
    WindowsProcessProbe(WindowsProcessProbe&&) = delete;
    WindowsProcessProbe& operator=(WindowsProcessProbe&&) = delete;

    [[nodiscard]] std::vector<ProcessCounters> enumerate() override;
    [[nodiscard]] ProcessCapabilities capabilities() const override;
    [[nodiscard]] uint64_t totalCpuTime() const override;
    [[nodiscard]] long ticksPerSecond() const override;
    [[nodiscard]] uint64_t systemTotalMemory() const override;

  private:
    bool m_HasPowerMonitoring = false;
    bool m_HasNetworkCounters = false;
    bool m_NetworkCountersAccessDenied = false; // True when EStats failed specifically due to access denied (privilege issue)
    mutable std::atomic<uint64_t> m_SyntheticEnergy{0};
    HMODULE m_IphlpModule = nullptr; // Non-null only when loaded by this class (must be freed in destructor)

    // EStats function signatures
    using GetPerTcpConnectionEStatsFn =
        DWORD(WINAPI*)(PMIB_TCPROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG);
    using SetPerTcpConnectionEStatsFn = DWORD(WINAPI*)(PMIB_TCPROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, ULONG);

    GetPerTcpConnectionEStatsFn m_GetPerTcpConnectionEStats = nullptr;
    SetPerTcpConnectionEStatsFn m_SetPerTcpConnectionEStats = nullptr;

    struct DetailCacheEntry
    {
        std::string user;
        std::string command;
        std::string status;
        std::string publisher;
        std::string processType;
        std::optional<std::int32_t> gdiObjectCount;
        std::chrono::steady_clock::time_point nextLightRefresh;
        std::chrono::steady_clock::time_point nextHeavyRefresh;
        std::uint64_t generation = 0;
    };

    /// Get detailed info for a single process
    [[nodiscard]] bool getProcessDetails(uint32_t pid, ProcessCounters& counters);

    /// Read total system CPU time
    [[nodiscard]] static uint64_t readTotalCpuTime();

    /// Detect if power monitoring is available
    [[nodiscard]] static bool detectPowerMonitoring();

    /// Read system-wide energy (microjoules) if available
    [[nodiscard]] uint64_t readSystemEnergy() const;

    /// Attribute energy to processes based on CPU usage
    void attributeEnergyToProcesses(std::vector<ProcessCounters>& processes) const;

    /// Detect ETW/EStats availability for per-process network counters
    [[nodiscard]] bool detectNetworkCounters();

    /// Collect cumulative network byte counts per PID (best-effort)
    [[nodiscard]] std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> collectNetworkByteCounts() const;

    void collectTcp4ByteCounts(std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>& perPid) const;

    void applyNetworkCounters(std::vector<ProcessCounters>& processes) const;
    std::unordered_map<std::uint64_t, DetailCacheEntry> m_DetailCache;
    std::uint64_t m_DetailCacheGeneration = 0;
};

} // namespace Platform
