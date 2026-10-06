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
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

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

    /// Every TCP socket's cumulative byte counters from Netlink INET_DIAG, each attributed to the
    /// process holding it via the inode-to-PID map. Raw readings only: Domain accumulates them into
    /// per-process totals (#1099).
    [[nodiscard]] SocketTrafficReading readSocketTraffic() const override;

    /// Test seam: attribute network traffic from `socketStats` (e.g. one over a scripted netlink
    /// transport) instead of the real socket. Not thread-safe; call before sampling starts.
    void setSocketStatsForTesting(std::shared_ptr<NetlinkSocketStats> socketStats);

    /// Test seam: the minimum age of the inode-to-PID map before readSocketTraffic() rebuilds it early
    /// for a socket that appeared unowned since it was built (#1259), normally
    /// Domain::Sampling::INODE_PID_CACHE_EARLY_REBUILD_MS. Not thread-safe; call before sampling starts.
    void setInodeMapEarlyRebuildIntervalForTesting(std::chrono::milliseconds interval)
    {
        m_InodeMapEarlyRebuildInterval = interval;
    }

    /// Test seam: called before each /proc/*/fd scan that rebuilds the inode-to-PID map, so a test can
    /// count the scans. Not thread-safe; set it before sampling starts.
    void setInodeMapScanHookForTesting(std::function<void()> hook)
    {
        m_InodeMapScanHook = std::move(hook);
    }
#endif

    /// Test seam: called at the end of enumerate(), after it captured the CPU total, so a test can
    /// change /proc/stat before totalCpuTime() is called and check the total was taken with the
    /// processes' stat reads (#1119). Not thread-safe against a concurrent enumerate(); set it before
    /// sampling starts.
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
    // call that follows it (NO_CAPTURED_TOTAL once taken, or before the first enumerate()). Read later -- after network attribution, whose
    // periodic inode->PID rebuild scans every /proc/*/fd -- the total's interval drifted from the processes' and every CPU% showed a
    // sawtooth (#1119). A failed read is captured as 0, not left as "none", so totalCpuTime() hands ProcessModel that 0 (it skips the
    // interval) instead of re-reading after the tail and reintroducing the skew.
    static constexpr std::uint64_t NO_CAPTURED_TOTAL = std::numeric_limits<std::uint64_t>::max();
    mutable std::atomic<std::uint64_t> m_TotalCpuTimeAtEnumerate = NO_CAPTURED_TOTAL;
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
    mutable std::shared_ptr<const std::unordered_map<std::uint64_t, SocketOwner>> m_InodeToPidCache;
    mutable std::chrono::steady_clock::time_point m_InodeToPidCacheTime;
    // When the last completed scan started, whether it replaced m_InodeToPidCache or came back empty:
    // a socket first seen unowned after this may have been opened since, so readSocketTraffic()
    // rebuilds the map early to attribute it (#1259).
    mutable std::chrono::steady_clock::time_point m_InodeToPidBuiltAt;
    // When the last rebuild was claimed, successful or not. No rebuild -- TTL, early, or the quick
    // retry after an empty scan -- starts within m_InodeMapEarlyRebuildInterval of it, so a /proc
    // whose scans keep coming back empty (every socket held by a process we can't read) is still
    // scanned at most once per interval.
    mutable std::chrono::steady_clock::time_point m_InodeToPidLastAttempt;
    std::function<void()> m_InodeMapScanHook; // See setInodeMapScanHookForTesting()
    // Defaults to Domain::Sampling::INODE_PID_CACHE_EARLY_REBUILD_MS, set in the constructor so this
    // Platform header doesn't include a Domain one.
    std::chrono::milliseconds m_InodeMapEarlyRebuildInterval{};

    // Sockets the last reading couldn't attribute, each with the sampledAt of the reading it was
    // first seen unowned in. Guarded by m_UnownedSocketsMutex.
    mutable std::mutex m_UnownedSocketsMutex;
    mutable std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> m_UnownedSocketsFirstSeen;
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

    /// Parse /proc/[pid]/io for I/O counters (requires permissions); unreadable sets ioCountersAvailable = false
    static void parseProcessIo(int32_t pid, ProcessCounters& counters, const std::filesystem::path& procRoot);

    /// Count file descriptors in /proc/[pid]/fd. Unreadable (permissions) sets handleCountAvailable and
    /// networkCountersAvailable = false: neither the count nor the process's connections can be known.
    /// A directory that can be listed but whose links can't be read (CAP_DAC_READ_SEARCH without
    /// CAP_SYS_PTRACE) sets only networkCountersAvailable = false: the count is known, but the socket
    /// inode-to-PID map reads those links, so none of its connections are attributed to it (#1328).
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

    using InodeToPidMap = std::unordered_map<std::uint64_t, SocketOwner>;

    /// The inode-to-PID map, and when the last completed scan started (m_InodeToPidBuiltAt).
    struct InodeToPidSnapshot
    {
        std::shared_ptr<const InodeToPidMap> map;
        std::chrono::steady_clock::time_point builtAt;
    };

    /// The inode-to-PID map, rebuilt from /proc/[pid]/fd when the last rebuild is at least `maxAge` old
    /// (see m_InodeToPidCache): the TTL normally, m_InodeMapEarlyRebuildInterval for an early rebuild.
    [[nodiscard]] InodeToPidSnapshot currentInodeToPidMap(std::chrono::milliseconds maxAge) const;

    /// Thread-safe copy of the current NetlinkSocketStats instance (see m_SocketStats).
    [[nodiscard]] std::shared_ptr<NetlinkSocketStats> socketStats() const;
#endif
};

} // namespace Platform
