#pragma once

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessProbe.h"
#include "Platform/PlatformConfig.h"
#include "Platform/ProcessArchitecture.h"
#include "Platform/ProcessTypes.h"

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
#include "Platform/Linux/NetlinkSocketStats.h"
#include "Platform/Linux/ProcPrivileges.h"
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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

    /// Testability constructor that also takes the cgroup filesystem root (normally /sys/fs/cgroup),
    /// where a process's freeze state is read for its "Suspended" status (#1183).
    LinuxProcessProbe(std::filesystem::path procRoot, std::filesystem::path powercapRoot, std::filesystem::path cgroupRoot);

    /// Testability constructor that also takes the CPU sysfs root (normally /sys/devices/system/cpu),
    /// whose `online` list each process's CPU affinity is limited to (#1384).
    LinuxProcessProbe(std::filesystem::path procRoot,
                      std::filesystem::path powercapRoot,
                      std::filesystem::path cgroupRoot,
                      std::filesystem::path cpuSysfsRoot);

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
    [[nodiscard]] MemoryRollupRead readMemoryRollup(std::int32_t pid, std::uint64_t startTimeTicks) const override;

    /// The /proc/<pid> files a process's record is built from, as text (#2728): what tasksmack-helper reads
    /// of a process hidepid hides from TaskSmack, which can't open any of them itself.
    // NOLINTBEGIN(readability-redundant-member-init) - {} lets designated initializers omit a field without a warning
    struct ProcFileTexts
    {
        std::string stat{};
        std::string statm{};
        std::string status{};
        std::optional<std::string> cmdline{}; ///< nullopt when it couldn't be read; empty for a kernel thread
        std::string io{};                     ///< Empty when unreadable: its I/O is then unavailable
        std::string cgroup{};
    };
    // NOLINTEND(readability-redundant-member-init)

    /// @p pid's record from @p files alone, through the same parsers as enumerate(): nullopt when its stat
    /// doesn't parse. What the files can't say is left unknown: its FD count and network counters (the
    /// helper's counters read supplies those) and its architecture.
    [[nodiscard]] std::optional<ProcessCounters> processFromFiles(std::int32_t pid, const ProcFileTexts& files) const;

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

    /// Test seam: how old the inode-to-PID map gets before it is rebuilt, normally
    /// Domain::Sampling::INODE_PID_CACHE_TTL_MS. Not thread-safe; call before sampling starts.
    void setInodeMapTtlForTesting(std::chrono::milliseconds ttl)
    {
        m_InodeMapTtl = ttl;
    }
#endif

    /// Test seam: the longest a cached command line is reused before it is read again (#1425),
    /// normally Domain::Sampling::PROCESS_CMDLINE_CACHE_TTL_MS. Not thread-safe; call before
    /// sampling starts.
    void setCmdlineCacheTtlForTesting(std::chrono::milliseconds ttl)
    {
        m_CmdlineCacheTtl = ttl;
    }

    /// Test seam: the host machine processes' architectures are compared against (#2394), normally
    /// uname()'s. Not thread-safe; call before sampling starts.
    void setHostMachineForTesting(ProcessArchitecture::Machine host)
    {
        m_HostMachine = host;
    }

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
    std::filesystem::path m_CgroupRoot;
    std::filesystem::path m_CpuSysfsRoot;
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
    // How /proc is mounted (hidepid, gid=) and whether TaskSmack is in its group: read once at
    // construction, for the PROCESSES_HIDDEN privilege bit (#2303). Remounting /proc is rare enough.
    std::optional<ProcPrivileges::ProcMount> m_ProcMount;
    bool m_InProcMountGroup = false;
    std::string m_PowerCapPath;
    std::uint64_t m_PowerCapMaxRangeUj = 0; // max_energy_range_uj, where the counter wraps (0: unknown)
    /// A multi-socket machine's other package counters (package-1 and up), read with the first (#2444).
    struct OtherPackageCounter
    {
        std::string path;
        std::uint64_t maxRangeUj = 0;
    };
    std::vector<OtherPackageCounter> m_OtherPackages;

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
    std::string m_OwnNetNamespace; // "net:[...]" of this process; empty when unreadable (#2398)

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
    // Defaults to Domain::Sampling::INODE_PID_CACHE_TTL_MS, likewise. See setInodeMapTtlForTesting().
    std::chrono::milliseconds m_InodeMapTtl{};

    // Sockets the last reading couldn't attribute, each with the sampledAt of the reading it was
    // first seen unowned in. Guarded by m_UnownedSocketsMutex.
    mutable std::mutex m_UnownedSocketsMutex;
    mutable std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> m_UnownedSocketsFirstSeen;
#endif

    // Command lines by PID (#1425). A command line is set at exec, so it is read once and reused by
    // the samples after it, until its entry expires (at most m_CmdlineCacheTtl): a process that
    // rewrites its argv (a process title) is still picked up. An entry is only used for the process
    // it was read from: the same start time (not a later process that reused the PID) and the same
    // comm (exec keeps both PID and start time, but gives the process a new comm).
    struct CmdlineCacheEntry
    {
        std::uint64_t startTimeTicks = 0;
        std::string comm;    // The name from stat it was read with
        std::string name;    // The full name resolved from it (ProcessName.h), or empty to keep comm
        std::string command; // The command shown
        std::chrono::steady_clock::time_point expiresAt;
    };
    using CmdlineCache = std::unordered_map<std::int32_t, CmdlineCacheEntry>;
    // Each enumerate() takes the cache for its pass and puts back only the entries of the processes it
    // saw, so an exited process's entry goes with the pass that no longer lists it: the cache holds at
    // most one entry per live process. A concurrent enumerate() finds it empty and reads every command
    // line, as before the cache. Guarded by m_CmdlineCacheMutex, held only to take and put it back.
    std::mutex m_CmdlineCacheMutex;
    CmdlineCache m_CmdlineCache;
    // Defaults to Domain::Sampling::PROCESS_CMDLINE_CACHE_TTL_MS. See setCmdlineCacheTtlForTesting().
    std::chrono::milliseconds m_CmdlineCacheTtl{};

    // The machine this kernel runs (uname), which a process's architecture is compared against (#2394).
    ProcessArchitecture::Machine m_HostMachine = ProcessArchitecture::Machine::Unknown;

    // Architectures by PID (#2394), read once per process from the ELF header of /proc/[pid]/exe: an entry is
    // used only for the process it was read from (the same start time and comm, as the command-line cache's;
    // an exec changes the comm). Taken and put back by each enumerate() as m_CmdlineCache is, so it holds at
    // most one entry per live process. Guarded by m_ArchitectureCacheMutex.
    struct ArchitectureCacheEntry
    {
        std::uint64_t startTimeTicks = 0;
        std::string comm;
        std::string architecture; // ElfArchitecture::linuxArchitectureLabel(); empty if unreadable
    };
    using ArchitectureCache = std::unordered_map<std::int32_t, ArchitectureCacheEntry>;
    std::mutex m_ArchitectureCacheMutex;
    ArchitectureCache m_ArchitectureCache;

    /// Set the architecture from previous's entry for this process, else from its executable (through
    /// pidDirFd); the process's entry moves to seen. Call before the command line renames the process.
    void readProcessArchitecture(int pidDirFd, ProcessCounters& counters, ArchitectureCache& previous, ArchitectureCache& seen) const;

    /// The architecture of the executable /proc/[pid]/exe (through pidDirFd) names, on a host machine:
    /// its ELF header's first bytes, and its path for an emulator's name. Empty if it can't be read.
    [[nodiscard]] static std::string readExecutableArchitecture(int pidDirFd, ProcessArchitecture::Machine host);

    /// Parse the stat file in the /proc/[pid] directory `pidDirFd` is open on, for process `pid`.
    [[nodiscard]] bool parseProcessStat(int pidDirFd, int32_t pid, ProcessCounters& counters) const;
    /// The stat file's text applied to `counters` for process `pid`; false when it doesn't parse.
    [[nodiscard]] bool applyStat(std::string_view stat, int32_t pid, ProcessCounters& counters) const;

    /// Parse /proc/[pid]/statm (through `pidDirFd`) for memory info
    void parseProcessStatm(int pidDirFd, ProcessCounters& counters) const;
    /// The statm file's text applied to `counters` (empty: unchanged).
    void applyStatm(std::string_view statm, ProcessCounters& counters) const;

    /// Parse /proc/[pid]/status (through `pidDirFd`) for owner (UID) info and CPU affinity
    /// (Cpus_allowed_list). The affinity is ANDed with `onlineCpus` when that is known (see readOnlineCpus()).
    static void parseProcessStatus(int pidDirFd, int32_t pid, ProcessCounters& counters, const std::optional<CpuAffinity>& onlineCpus);
    /// The status file's text applied to `counters`: the owner, peak RSS (after statm), private and swapped
    /// memory, and the CPU affinity ANDed with `onlineCpus`. A line the text cuts off is ignored.
    static void applyStatus(std::string_view text, ProcessCounters& counters, const std::optional<CpuAffinity>& onlineCpus);

    /// The online CPUs, from <cpuSysfsRoot>/online, or nullopt if it can't be read or parsed (#1384).
    [[nodiscard]] static std::optional<CpuAffinity> readOnlineCpus(const std::filesystem::path& cpuSysfsRoot);

    /// Set the command (and the full name, where comm was cut) from /proc/[pid]/cmdline, through
    /// `pidDirFd`: from `previous`'s entry for this process while it is fresh, otherwise read. The
    /// process's entry, if it has one now, moves to `seen` (see m_CmdlineCache).
    void readProcessCommand(int pidDirFd,
                            ProcessCounters& counters,
                            CmdlineCache& previous,
                            CmdlineCache& seen,
                            std::chrono::steady_clock::time_point now) const;

    /// Read /proc/[pid]/cmdline (through `pidDirFd`) whole: nullopt if it can't be read.
    [[nodiscard]] static std::optional<std::vector<char>> readCmdline(int pidDirFd);

    /// Set the command, and the full name where comm was cut, from a raw command line.
    static void applyCmdline(ProcessCounters& counters, const std::vector<char>& rawCmdline);

    /// Parse /proc/[pid]/io (through `pidDirFd`) for I/O counters (requires permissions); unreadable
    /// sets ioCountersAvailable = false
    static void parseProcessIo(int pidDirFd, ProcessCounters& counters);
    /// The io file's text applied to `counters`: empty or unparsable sets ioCountersAvailable = false.
    static void applyIo(std::string_view ioText, ProcessCounters& counters);

    /// Count file descriptors in /proc/[pid]/fd, through `pidDirFd`. Unreadable (permissions) sets
    /// handleCountAvailable and networkCountersAvailable = false: neither the count nor the process's
    /// connections can be known. A directory that can be listed but whose links can't be read
    /// (CAP_DAC_READ_SEARCH without CAP_SYS_PTRACE) sets only networkCountersAvailable = false: the
    /// count is known, but the socket inode-to-PID map reads those links, so none of its connections
    /// are attributed to it (#1328). With `readEveryLink`, every link is read and each socket's inode
    /// passed to `onSocket`: the inode-to-PID map from the same walk (#1426).
    template<typename OnSocket>
    static void countProcessFds(int pidDirFd, ProcessCounters& counters, bool readEveryLink, OnSocket&& onSocket);
    /// Whether the process whose /proc/[pid] directory @p pidDirFd is open on is in another network
    /// namespace than this one; false when either link can't be read (#2398).
    [[nodiscard]] bool inAnotherNetworkNamespace(int pidDirFd) const;

    /// Check if we can read I/O counters using the injected proc root
    [[nodiscard]] static bool checkIoCountersAvailability(const std::filesystem::path& procRoot);

    /// Transparent string hash, so a map keyed by std::string can be searched with a string_view.
    struct StringHash
    {
        using is_transparent = void; // NOLINT(readability-identifier-naming) - the standard library's name
        [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept
        {
            return std::hash<std::string_view>{}(text);
        }
    };
    /// What a set of cgroups (a /proc/[pid]/cgroup file's contents) says of its processes.
    struct CgroupFacts
    {
        bool frozen = false;
        std::string container; // CgroupContainer::containerFromCgroups()
        std::string services;  // CgroupService::serviceFromCgroups() (#1886)
    };
    /// The facts of each set of cgroups seen, within one pass.
    using FactsByCgroup = std::unordered_map<std::string, CgroupFacts, StringHash, std::equal_to<>>;

    /// Fill `counters`' status (Suspended, from the cgroup freezer) and container from its cgroups;
    /// /proc/[pid]/cgroup is read through `pidDirFd`. Both are worked out once per pass for each
    /// distinct set of cgroups (`factsByCgroup`): processes sharing a cgroup share the answer.
    static void
    readCgroupFacts(int pidDirFd, const std::filesystem::path& cgroupRoot, FactsByCgroup& factsByCgroup, ProcessCounters& counters);
    /// The same, from the cgroup file's text (empty: unchanged).
    static void applyCgroupFacts(std::string_view cgroups,
                                 const std::filesystem::path& cgroupRoot,
                                 FactsByCgroup& factsByCgroup,
                                 ProcessCounters& counters);

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

    /// Beside a first counter at <zones>/intel-rapl:0, the other readable top-level zones there that are
    /// CPU packages (their name "package-N"; not psys, the whole platform, which would count twice), in
    /// zone order (#2444).
    void detectOtherPackages(const std::filesystem::path& firstZone);

    /// Read the package energy counter in microjoules, or nullopt if it can't be read.
    [[nodiscard]] std::optional<uint64_t> readSystemEnergy() const;

    /// A RAPL counter file (energy_uj, max_energy_range_uj) as a number, or nullopt.
    [[nodiscard]] static std::optional<uint64_t> readEnergyFile(const std::string& path);

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

    /// Claim the next rebuild of the inode-to-PID map if the last is at least `maxAge` old and none
    /// was attempted within m_InodeMapEarlyRebuildInterval; the caller then builds and publishes it.
    /// Returns the current (possibly stale) snapshot, and whether the rebuild is the caller's.
    [[nodiscard]] std::pair<InodeToPidSnapshot, bool> claimInodeMapRebuild(std::chrono::milliseconds maxAge) const;

    /// Publish a rebuilt inode-to-PID map whose scan started at `scanStart`, and return the snapshot
    /// now current. An empty map keeps the previous one, with a quick retry.
    InodeToPidSnapshot publishInodeToPidMap(InodeToPidMap rebuilt, std::chrono::steady_clock::time_point scanStart) const;

    /// enumerate()'s rebuild pass (#1426): count every process's FDs, reading every link, and publish
    /// the inode-to-PID map of the sockets among them. `procDirFd` is open on the proc root (-1 if it
    /// couldn't be listed: the map is then published empty, which keeps the last one).
    void rebuildInodeMapWithFdCounts(int procDirFd, std::vector<ProcessCounters>& processes) const;

    /// Thread-safe copy of the current NetlinkSocketStats instance (see m_SocketStats).
    [[nodiscard]] std::shared_ptr<NetlinkSocketStats> socketStats() const;
#endif
};

} // namespace Platform
