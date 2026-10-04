#pragma once

#include "Platform/IProcessProbe.h"
#include "Platform/PlatformConfig.h"

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
#include "Platform/Linux/NetlinkSocketStats.h"

#include <chrono>
#include <unordered_map>
#endif

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace Platform
{

/// Linux implementation of IProcessProbe.
/// Reads from /proc filesystem.
class LinuxProcessProbe : public IProcessProbe
{
  public:
    LinuxProcessProbe();

    /// Testability constructor: reads from a custom proc root instead of /proc.
    /// Useful for unit tests that supply synthetic /proc content.
    explicit LinuxProcessProbe(std::filesystem::path procRoot);

    /// Testability constructor that also takes the powercap root (normally /sys/class/powercap),
    /// so power detection can be exercised against fixture files.
    LinuxProcessProbe(std::filesystem::path procRoot, std::filesystem::path powercapRoot);

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
    [[nodiscard]] std::optional<PackageEnergyReading> readPackageEnergy() const override;

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    /// Set the socket stats cache TTL (Linux only)
    /// @param ttlMs Time-to-live in milliseconds for cached socket stats
    /// Use this to override the default cache TTL at runtime (e.g., from user config)
    void setSocketStatsCacheTtl(std::chrono::milliseconds ttlMs) override;
#endif

    /// Test seam: called by enumerate() where its variable-latency tail (network attribution) runs,
    /// so a test can change /proc/stat during that tail and check the CPU total was taken before it
    /// (#1119). Not thread-safe against a concurrent enumerate(); set it before sampling starts.
    void setEnumerateTailHookForTesting(std::function<void()> hook)
    {
        m_EnumerateTailHook = std::move(hook);
    }

  private:
    std::filesystem::path m_ProcRoot;
    std::filesystem::path m_PowercapRoot;
    long m_TicksPerSecond;
    uint64_t m_PageSize;
    uint64_t m_BootTimeEpoch = 0;                            // System boot time (Unix epoch seconds)
    mutable std::once_flag m_IoCountersCheckFlag;            // Thread-safe one-time initialization
    mutable std::atomic<bool> m_IoCountersAvailable = false; // Cached capability check (atomic for thread-safe read)
    // Total CPU time read straight after enumerate()'s per-process stat pass, for the totalCpuTime()
    // call that follows it (0 once taken). Read later -- after network attribution, whose periodic
    // inode->PID rebuild scans every /proc/*/fd -- the total's interval drifted from the processes'
    // and every CPU% showed a sawtooth (#1119).
    mutable std::atomic<std::uint64_t> m_TotalCpuTimeAtEnumerate = 0;
    std::function<void()> m_EnumerateTailHook; // See setEnumerateTailHookForTesting()
    bool m_HasPowerCap = false;
    std::string m_PowerCapPath;
    std::uint64_t m_PowerCapMaxRangeUj = 0; // max_energy_range_uj, where the counter wraps (0: unknown)

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    // Per-process network monitoring via Netlink INET_DIAG. m_SocketStatsMutex guards
    // m_SocketStats so setSocketStatsCacheTtl() can publish a new instance concurrently
    // with enumerate() running on the background sampler thread: socketStats() copies out
    // a stable shared_ptr under the lock, so a concurrent TTL change can never race a
    // reader's in-flight dereference or destroy the object out from under it. (libc++ 22
    // does not yet implement the std::atomic<std::shared_ptr<T>> specialization, so a
    // plain mutex is used instead.)
    mutable std::mutex m_SocketStatsMutex;
    std::shared_ptr<NetlinkSocketStats> m_SocketStats;
    bool m_HasNetworkCounters = false;

    // Inode-to-PID cache: refreshed on a TTL basis to avoid scanning
    // /proc/[pid]/fd/* every enumerate(). The claim timestamp is set under the
    // initial lock so only one thread rebuilds per TTL window while others
    // continue using the previous cache snapshot. The map is stored behind a
    // shared_ptr so callers copy the pointer O(1) rather than the entire map.
    // m_InodePidCacheMutex guards publication of both cache members; necessary
    // because enumerate() may be called concurrently from multiple threads.
    mutable std::mutex m_InodePidCacheMutex;
    mutable std::shared_ptr<const std::unordered_map<std::uint64_t, std::int32_t>> m_InodeToPidCache;
    mutable std::chrono::steady_clock::time_point m_InodeToPidCacheTime;
#endif

    /// Parse /proc/[pid]/stat for a single process
    [[nodiscard]] bool parseProcessStat(int32_t pid, ProcessCounters& counters) const;

    /// Parse /proc/[pid]/statm for memory info
    void parseProcessStatm(int32_t pid, ProcessCounters& counters) const;

    /// Parse /proc/[pid]/status for owner (UID) info
    static void parseProcessStatus(int32_t pid, ProcessCounters& counters, const std::filesystem::path& procRoot);

    /// Parse /proc/[pid]/cmdline for full command line
    static void parseProcessCmdline(int32_t pid, ProcessCounters& counters, const std::filesystem::path& procRoot);

    /// Parse CPU affinity mask for a process using sched_getaffinity
    static void parseProcessAffinity(int32_t pid, ProcessCounters& counters);

    /// Parse /proc/[pid]/io for I/O counters (requires permissions)
    static void parseProcessIo(int32_t pid, ProcessCounters& counters, const std::filesystem::path& procRoot);

    /// Count file descriptors in /proc/[pid]/fd (may fail due to permissions)
    static void countProcessFds(int32_t pid, ProcessCounters& counters, const std::filesystem::path& procRoot);

    /// Check if we can read I/O counters using the injected proc root
    [[nodiscard]] static bool checkIoCountersAvailability(const std::filesystem::path& procRoot);

    /// Get process status from cgroups (Suspended state detection)
    [[nodiscard]] static std::string getProcessStatus(int32_t pid, const std::filesystem::path& procRoot);

    /// Read total CPU time from /proc/stat
    [[nodiscard]] uint64_t readTotalCpuTime() const;

    /// /proc/stat's first line: all CPU time, and the part processes' utime + stime account for.
    struct CpuTimes
    {
        uint64_t total = 0;
        uint64_t busy = 0;
    };
    [[nodiscard]] std::optional<CpuTimes> readCpuTimes() const;

    /// Read system boot time from /proc/stat (returns Unix epoch seconds, 0 if unavailable)
    [[nodiscard]] static uint64_t readBootTime(const std::filesystem::path& procRoot);

    /// Find a RAPL package energy file this process can actually read (not just one that exists:
    /// energy_uj is root-only on current kernels, #1103), and its wrap point.
    [[nodiscard]] bool detectPowerCap();

    /// Read the package energy counter in microjoules, or nullopt if it can't be read.
    [[nodiscard]] std::optional<uint64_t> readSystemEnergy() const;

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    /// Attribute network bytes to processes using Netlink socket stats
    void attributeNetworkToProcesses(std::vector<ProcessCounters>& processes) const;

    /// Thread-safe copy of the current NetlinkSocketStats instance (see m_SocketStats).
    [[nodiscard]] std::shared_ptr<NetlinkSocketStats> socketStats() const;
#endif
};

} // namespace Platform
