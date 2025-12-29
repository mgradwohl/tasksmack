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
    ~WindowsProcessProbe() override = default;

    WindowsProcessProbe(const WindowsProcessProbe&) = delete;
    WindowsProcessProbe& operator=(const WindowsProcessProbe&) = delete;
    WindowsProcessProbe(WindowsProcessProbe&&) = default;
    WindowsProcessProbe& operator=(WindowsProcessProbe&&) = default;

    [[nodiscard]] std::vector<ProcessCounters> enumerate() override;
    [[nodiscard]] ProcessCapabilities capabilities() const override;
    [[nodiscard]] uint64_t totalCpuTime() const override;
    [[nodiscard]] long ticksPerSecond() const override;
    [[nodiscard]] uint64_t systemTotalMemory() const override;

  private:
    bool m_HasPowerMonitoring = false;
    bool m_HasNetworkCounters = false;
    mutable uint64_t m_SyntheticEnergy = 0;

    using GetPerTcpConnectionEStatsFn =
        DWORD(WINAPI*)(PMIB_TCPROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG);

    GetPerTcpConnectionEStatsFn m_GetPerTcpConnectionEStats = nullptr;

    /// Get detailed info for a single process
    [[nodiscard]] static bool getProcessDetails(uint32_t pid, ProcessCounters& counters);

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
};

} // namespace Platform
