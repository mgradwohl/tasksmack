#pragma once

#include "Platform/CpuAffinity.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{

/// The OS's own priority class for a process, on a platform that schedules by class (Windows,
/// GetPriorityClass()), lowest first. None: the platform has no classes (Linux; nice is the
/// priority) or the class could not be read. A raw OS value, not a label: Domain names it (#1280).
enum class PriorityClass : std::uint8_t
{
    None,
    Idle,
    BelowNormal,
    Normal,
    AboveNormal,
    High,
    Realtime,
};

/// Raw counters from OS - no computed values.
/// Probes populate this; domain computes deltas and rates.
struct ProcessCounters
{
    std::int32_t pid = 0;
    std::int32_t parentPid = 0;
    std::string name;
    std::string command;  // Full command line
    std::string user;     // Username (owner) of the process
    std::string realUser; // Who started it, when not `user`: a setuid program's caller (Linux, #2475); else empty
    char state = '?';     // Raw state character from OS (e.g., 'R', 'S', 'Z')
    // Windows' System Idle Process (PID 0): its userTime/systemTime are the CPUs' idle time, not work.
    // Package energy attribution leaves it out (#2587); Linux lists no such process.
    bool cpuTimeIsIdle = false;
    std::string status;    // Process status (e.g., "Suspended", "Efficiency Mode")
    std::int32_t nice = 0; // Nice value (-20 to 19 on Linux)
    // Windows: the class nice stands for, which nice alone can't name (Realtime and High both sit
    // below HIGH_THRESHOLD, #1280). None on Linux, or when the class was not read.
    PriorityClass priorityClass = PriorityClass::None;

    std::uint64_t startTimeTicks = 0; // For PID reuse detection (raw platform ticks)
    std::uint64_t startTimeEpoch = 0; // Process start time (Unix epoch seconds)

    // CPU time (cumulative ticks/jiffies)
    std::uint64_t userTime = 0;
    std::uint64_t systemTime = 0;

    // Memory (bytes)
    // The Memory column (#1184). Linux: the resident field of /proc/[pid]/statm (pages x page size;
    // /proc/[pid]/stat's rss if statm can't be read), the resident set, shared pages included. Windows: the
    // private working set, Task Manager's default "Memory (private working set)".
    std::uint64_t rssBytes = 0;
    std::uint64_t peakRssBytes = 0; // OS-reported peak resident size, 0 = unknown (Linux: VmHWM, which resets on
                                    // exec; Windows: PeakWorkingSetSize). Domain keeps the highest peak it observed.
    // The Private column (#1879, #1885): memory no other process shares. Linux: RssAnon from
    // /proc/[pid]/status, the resident anonymous pages (heap, stacks, private mappings). Windows: not
    // filled; its private bytes are the commit charge, already virtualBytes (#1879).
    std::uint64_t privateBytes = 0;
    // The Swap column (#1879, #1885): the part of it swapped out. Linux: VmSwap from /proc/[pid]/status.
    // Windows: not reported (ProcessCapabilities::hasSwapBytes is false), always 0.
    std::uint64_t swapBytes = 0;
    // The Virtual column (#1184). Linux: vsize from /proc/[pid]/stat, the whole address space. Windows: the commit size
    // (PrivateUsage), as Task Manager shows it; its VirtualSize counts reservations, terabytes for
    // any process using Control Flow Guard.
    std::uint64_t virtualBytes = 0;
    // Shared memory: the resident shared pages (statm) on Linux; on Windows the shareable working set,
    // WorkingSetSize - WorkingSetPrivateSize: pages that can be shared with other processes (#2583).
    std::uint64_t sharedBytes = 0;

    // Optional fields (check capabilities)
    std::uint64_t readBytes = 0;
    std::uint64_t writeBytes = 0;
    std::int32_t threadCount = 0;
    std::int32_t handleCount = 0;     // Open handles (Windows) or file descriptors (Linux)
    std::uint64_t pageFaultCount = 0; // Total page faults (minor + major on Linux)
    CpuAffinity cpuAffinity;          // Logical processors it may run on (empty = not available)

    // Network counters (cumulative bytes). A probe that reports per-connection readings instead
    // (IProcessProbe::readSocketTraffic()) leaves these 0; Domain fills them from the readings.
    std::uint64_t netSentBytes = 0;
    std::uint64_t netReceivedBytes = 0;
    // When the network counters were read from the OS, as std::chrono::steady_clock nanoseconds since
    // its epoch; 0 = read with this refresh. A probe that caches its network query for longer than a
    // refresh interval sets it, so rates are taken over the time between real reads (#1063 review).
    std::uint64_t netSampleTimeNs = 0;

    // Power usage (optional, platform-dependent; see ProcessCapabilities::hasPowerUsage)
    // On Linux: from powercap sysfs (per-package energy counters), shared out by CPU time
    // On Windows: not populated; always 0 (#1028)
    std::uint64_t energyMicrojoules = 0; // Cumulative energy consumption in microjoules

    // Publisher / vendor info (optional, Windows-only via PE version info)
    std::string publisher; // CompanyName from PE file version info (empty if not available)

    // Container (optional, Linux-only from /proc/[pid]/cgroup; see CgroupContainer.h)
    // "<runtime>:<short id or name>", e.g. "docker:1a2b3c4d5e6f" (empty if in none)
    std::string container;

    // The services the process runs (#1880, #1886). Linux: its systemd service, from /proc/[pid]/cgroup
    // (see CgroupService.h), e.g. "sshd.service" (empty if in none). Windows leaves it empty: its services
    // are matched to processes by PID from the service list instead (Domain::ProcessModel).
    std::string services;

    // Process type classification (optional, Windows-only)
    // Values: "App", "Background Process", "Windows Process" (empty if not available)
    std::string processType;

    // The architecture the process runs as (#2394), as ProcessArchitecture::architectureLabel() words it:
    // "x64", "ARM64", "ARM64EC", "x64 (emulated)", "x86 (WOW64)" (empty if it couldn't be read). Windows:
    // GetProcessInformation(ProcessMachineTypeInfo), else IsWow64Process2(); Linux: the ELF header of
    // /proc/[pid]/exe. Read once per process, since it never changes.
    std::string architecture;

    // GDI object count (optional, Windows-only via GetGuiResources).
    // std::nullopt means the probe could not open the process with the required rights.
    // A stored value of 0 means the process is accessible but owns no GDI objects.
    std::optional<std::int32_t> gdiObjectCount;

    // Whether the probe could read these for this process (#1110). A probe sets one false when the
    // read failed -- typically for lack of rights, and the value beside it is then a placeholder 0, not a measurement. On Linux, for
    // another user's process: listing /proc/[pid]/fd (handleCount) needs CAP_DAC_READ_SEARCH; /proc/[pid]/io (I/O) and reading the
    // fd links that network attribution uses also need CAP_SYS_PTRACE (root has both unless they are dropped). Windows (#1285) reads
    // handles and I/O for every process from its bulk snapshot, and network bytes for every process or, without per-process network
    // counters (not elevated), for none. The defaults suit a probe that reads every process it lists; a field the probe never fills
    // at all is reported by ProcessCapabilities instead.
    bool handleCountAvailable = true;     // handleCount
    bool ioCountersAvailable = true;      // readBytes / writeBytes
    bool networkCountersAvailable = true; // netSentBytes / netReceivedBytes: the process's connections
                                          // could be attributed to it (Linux: from its /proc/[pid]/fd;
                                          // Windows: per-process network counters are on)

    // This refresh couldn't read the process, so this is the last read's record, carried over (#3031):
    // Linux's hidepid-hidden processes, when the helper's listing was skipped because the helper was busy
    // with another call. The Domain keeps the process and its last published values, and measures its
    // next real read against the last real one.
    bool heldOver = false;
};

/// One connection's cumulative byte counters as the OS reports them, and the process it belongs to.
struct SocketTrafficSample
{
    std::uint64_t key = 0; // Stable identity of the connection for its lifetime: the socket inode on Linux
    std::int32_t pid = 0;  // Owning process; 0 = not attributed (yet)
    // The owning process's start time, in the same raw ticks as ProcessCounters::startTimeTicks, so a
    // process that reused `pid` isn't credited with this connection's bytes (#1336). 0 = unknown: the
    // connection is then matched to a process by PID alone. Linux reads it from /proc/[pid]/stat when
    // building the inode-to-PID map; Windows reports 0 (its TCP tables give only the owning PID).
    std::uint64_t ownerStartTimeTicks = 0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t bytesSent = 0;
    // False for a connection present in the OS table whose counters couldn't be read this time
    // (Windows: a failed or garbage EStats read); its byte fields are then ignored. Reported rather
    // than left out so Domain doesn't take it as closed, and back as new (#1256).
    bool readable = true;
};

/// A process's /proc/<pid> files as text (Linux, #2728): what tasksmack-helper reads of a process hidepid
/// hides from TaskSmack, which can't open any of them itself, for IProcessProbe::processFromFiles().
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

/// One complete reading of every connection's raw byte counters (IProcessProbe::readSocketTraffic()).
/// Domain turns successive readings into monotonic per-process counters (#1099).
struct SocketTrafficReading
{
    std::vector<SocketTrafficSample> sockets;
    /// When the connections were read from the OS, as std::chrono::steady_clock nanoseconds since its
    /// epoch. A probe that caches its query returns the same time for the same reading. 0 = no
    /// complete reading this time (unsupported, failed, or interrupted): `sockets` is then empty and
    /// must not be treated as "every connection closed".
    std::uint64_t sampleTimeNs = 0;
};

/// The permissions a ProcessCapabilities::missingPrivileges bit stands for (Linux, #2303).
namespace MissingPrivilege
{
/// Reading other users' FD counts, I/O and network attribution: CAP_DAC_READ_SEARCH + CAP_SYS_PTRACE.
inline constexpr std::uint8_t READ_OTHERS = 1U << 0U;
/// Ending, suspending or resuming other users' processes: CAP_KILL.
inline constexpr std::uint8_t SIGNAL_OTHERS = 1U << 1U;
/// Raising priority, the Realtime I/O class, other users' priority and affinity: CAP_SYS_NICE.
inline constexpr std::uint8_t RAISE_PRIORITY = 1U << 2U;
/// /proc is mounted with hidepid, so other users' processes can't be read and aren't listed.
inline constexpr std::uint8_t PROCESSES_HIDDEN = 1U << 3U;
} // namespace MissingPrivilege

/// Reports what this platform's probe supports.
/// UI can degrade gracefully for missing capabilities.
struct ProcessCapabilities
{
    bool hasIoCounters = false;
    bool hasThreadCount = false;
    bool hasHandleCount = false; // Whether handle/FD count is available
    bool hasUserSystemTime = true;
    bool hasStartTime = true;
    bool hasUser = false;               // Whether process owner/user is available
    bool hasCommand = false;            // Whether full command line is available
    bool hasNice = false;               // Whether nice/priority value is available
    bool hasPageFaults = false;         // Whether page fault count is available
    bool hasPeakRss = false;            // Whether peak working set is available
    bool hasPrivateBytes = false;       // Whether ProcessCounters::privateBytes is filled (Linux: RssAnon, #1885;
                                        // not on Windows, where it would repeat Virtual, #1879)
    bool hasSwapBytes = false;          // Whether ProcessCounters::swapBytes is filled (Linux: VmSwap, #1885; Windows
                                        // has no per-process figure, #1879)
    bool hasMemoryRollup = false;       // Whether IProcessProbe::readMemoryRollup() reads PSS/USS (Linux: smaps_rollup, #1885)
    bool hasCpuAffinity = false;        // Whether CPU affinity mask is available
    bool hasNetworkCounters = false;    // Whether per-process network counters are available
    bool hasUdpNetworkCounters = false; // Whether those counters include UDP (QUIC/HTTP3, WebRTC, games, DNS).
                                        // False: TCP only (#1101). Linux: the kernel's sock_diag reports byte
                                        // counts for TCP sockets only. Windows: TCP EStats only; it has
                                        // no user-mode per-process UDP byte counters (#1258).
    bool hasPowerUsage = false;         // Whether power consumption metrics are available
    bool hasStatus = false;             // Whether process status (Suspended, Efficiency Mode) is available
    bool hasPublisher = false;          // Whether publisher/vendor string is available (Windows PE version info)
    bool hasContainer = false;          // Whether the container a process runs in is available (Linux cgroups)
    bool hasServices = false;           // Whether the services a process runs are available: Linux, the systemd
                                        // unit from its cgroups (ProcessCounters::services, #1886); Windows, set by
                                        // Domain::ProcessModel once a service list with PIDs is attached (#1880)
    bool hasProcessType = false;        // Whether process type classification is available (Windows: App/Background/Windows)
    bool hasArchitecture = false;       // Whether ProcessCounters::architecture is filled (#2394)
    bool hasGdiObjects = false;         // Whether GDI object count is available (Windows-only via GetGuiResources)
    bool hasReducedPrivileges = false;  // True when the process lacks the privileges to read some data, or act
                                        // on other users' processes (not necessarily curable by elevation: sudo
                                        // can't restore capabilities a container or service dropped).
                                        // Linux: missingPrivileges isn't empty (ProcPrivileges::missingPrivileges()).
                                        // Windows: non-admin AND EStats was specifically denied (ERROR_ACCESS_DENIED).
                                        //          Remains false when EStats is simply unsupported, because
                                        //          running as Administrator would not restore those counters.
    // True when per-process network counters were denied although the process already has the
    // privileges they need, so elevating would not help. Windows: elevated, yet TCP EStats access
    // denied (a policy or a driver, #1358). Never true together with hasNetworkCounters or
    // hasReducedPrivileges. Linux: always false (its gaps are hasReducedPrivileges).
    bool networkCountersBlocked = false;
    bool hasSharedMemory = false; // Whether ProcessCounters::sharedBytes is filled (Linux: statm; Windows: shareable working set)
    // Which permissions are missing, as MissingPrivilege bits (#2303), so the Limited Data notice and the
    // status bar can say what each one blocks. Linux only; 0 elsewhere (Windows keeps its own notice).
    std::uint8_t missingPrivileges = 0;
    // How many bits ProcessCounters::pageFaultCount is kept in by the OS before it wraps to 0 (#1184).
    // Linux: 64 (minflt + majflt, unsigned long). Windows: 32 (SYSTEM_PROCESS_INFORMATION's ULONG
    // PageFaultCount, which a long-lived process can pass). Domain takes deltas modulo 2^bits.
    std::uint8_t pageFaultCountBits = 64;

    friend bool operator==(const ProcessCapabilities&, const ProcessCapabilities&) = default;
};

} // namespace Platform
