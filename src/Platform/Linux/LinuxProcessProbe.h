#pragma once

#include "Platform/IProcessProbe.h"
#include "Platform/PlatformConfig.h"

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
#include "Platform/Linux/NetlinkSocketStats.h"
#endif

#include <atomic>
#include <memory>
#include <mutex>

namespace Platform
{

/// Linux implementation of IProcessProbe.
/// Reads from /proc filesystem.
class LinuxProcessProbe : public IProcessProbe
{
  public:
    LinuxProcessProbe();
    ~LinuxProcessProbe() override = default;

    LinuxProcessProbe(const LinuxProcessProbe&) = delete;
    LinuxProcessProbe& operator=(const LinuxProcessProbe&) = delete;
    // std::once_flag is not movable, so this type cannot be stored in move-requiring containers
    LinuxProcessProbe(LinuxProcessProbe&&) = delete;
    LinuxProcessProbe& operator=(LinuxProcessProbe&&) = delete;

    [[nodiscard]] std::vector<ProcessCounters> enumerate() override;
    [[nodiscard]] ProcessCapabilities capabilities() const override;
    [[nodiscard]] uint64_t totalCpuTime() const override;
    [[nodiscard]] long ticksPerSecond() const override;
    [[nodiscard]] uint64_t systemTotalMemory() const override;

  private:
    long m_TicksPerSecond;
    uint64_t m_PageSize;
    uint64_t m_BootTimeEpoch = 0;                            // System boot time (Unix epoch seconds)
    mutable std::once_flag m_IoCountersCheckFlag;            // Thread-safe one-time initialization
    mutable std::atomic<bool> m_IoCountersAvailable = false; // Cached capability check (atomic for thread-safe read)
    bool m_HasPowerCap = false;
    std::string m_PowerCapPath;

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    // Per-process network monitoring via Netlink INET_DIAG
    std::unique_ptr<NetlinkSocketStats> m_SocketStats;
    bool m_HasNetworkCounters = false;
#endif

    /// Parse /proc/[pid]/stat for a single process
    [[nodiscard]] bool parseProcessStat(int32_t pid, ProcessCounters& counters) const;

    /// Parse /proc/[pid]/statm for memory info
    void parseProcessStatm(int32_t pid, ProcessCounters& counters) const;

    /// Parse /proc/[pid]/status for owner (UID) info
    static void parseProcessStatus(int32_t pid, ProcessCounters& counters);

    /// Parse /proc/[pid]/cmdline for full command line
    static void parseProcessCmdline(int32_t pid, ProcessCounters& counters);

    /// Parse CPU affinity mask for a process using sched_getaffinity
    static void parseProcessAffinity(int32_t pid, ProcessCounters& counters);

    /// Parse /proc/[pid]/io for I/O counters (requires permissions)
    static void parseProcessIo(int32_t pid, ProcessCounters& counters);

    /// Count file descriptors in /proc/[pid]/fd (may fail due to permissions)
    static void countProcessFds(int32_t pid, ProcessCounters& counters);

    /// Check if we can read I/O counters (checks own process)
    [[nodiscard]] static bool checkIoCountersAvailability();

    /// Get process status from cgroups (Suspended state detection)
    [[nodiscard]] static std::string getProcessStatus(int32_t pid);

    /// Read total CPU time from /proc/stat
    [[nodiscard]] static uint64_t readTotalCpuTime();

    /// Read system boot time from /proc/stat (returns Unix epoch seconds, 0 if unavailable)
    [[nodiscard]] static uint64_t readBootTime();

    /// Check if RAPL powercap is available and find the path
    [[nodiscard]] bool detectPowerCap();

    /// Read system-wide energy from RAPL (returns microjoules, 0 if unavailable)
    [[nodiscard]] uint64_t readSystemEnergy() const;

    /// Attribute system energy to processes based on CPU usage
    void attributeEnergyToProcesses(std::vector<ProcessCounters>& processes) const;

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    /// Attribute network bytes to processes using Netlink socket stats
    void attributeNetworkToProcesses(std::vector<ProcessCounters>& processes) const;
#endif
};

} // namespace Platform
