#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{

/// Raw counters from OS - no computed values.
/// Probes populate this; domain computes deltas and rates.
struct ProcessCounters
{
    std::int32_t pid = 0;
    std::int32_t parentPid = 0;
    std::string name;
    std::string command;   // Full command line
    std::string user;      // Username (owner) of the process
    char state = '?';      // Raw state character from OS (e.g., 'R', 'S', 'Z')
    std::string status;    // Process status (e.g., "Suspended", "Efficiency Mode")
    std::int32_t nice = 0; // Nice value (-20 to 19 on Linux)

    std::uint64_t startTimeTicks = 0; // For PID reuse detection (raw platform ticks)
    std::uint64_t startTimeEpoch = 0; // Process start time (Unix epoch seconds)

    // CPU time (cumulative ticks/jiffies)
    std::uint64_t userTime = 0;
    std::uint64_t systemTime = 0;

    // Memory (bytes)
    std::uint64_t rssBytes = 0;
    std::uint64_t peakRssBytes = 0; // Peak resident size over the process's life, 0 = unknown (Linux: VmHWM;
                                    // Windows: PeakWorkingSetSize)
    std::uint64_t virtualBytes = 0;
    std::uint64_t sharedBytes = 0; // Shared memory (from statm on Linux)

    // Optional fields (check capabilities)
    std::uint64_t readBytes = 0;
    std::uint64_t writeBytes = 0;
    std::int32_t threadCount = 0;
    std::int32_t handleCount = 0;      // Open handles (Windows) or file descriptors (Linux)
    std::uint64_t pageFaultCount = 0;  // Total page faults (minor + major on Linux)
    std::uint64_t cpuAffinityMask = 0; // Bitmask of allowed CPU cores (0 = not available)

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

    // Process type classification (optional, Windows-only)
    // Values: "App", "Background Process", "Windows Process" (empty if not available)
    std::string processType;

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
    bool hasCpuAffinity = false;        // Whether CPU affinity mask is available
    bool hasNetworkCounters = false;    // Whether per-process network counters are available
    bool hasUdpNetworkCounters = false; // Whether those counters include UDP (QUIC/HTTP3, WebRTC, games, DNS).
                                        // False: TCP only (#1101). Linux: the kernel's sock_diag reports byte
                                        // counts for TCP sockets only. Windows: TCP EStats only, for now.
    bool hasPowerUsage = false;         // Whether power consumption metrics are available
    bool hasStatus = false;             // Whether process status (Suspended, Efficiency Mode) is available
    bool hasPublisher = false;          // Whether publisher/vendor string is available (Windows PE version info)
    bool hasProcessType = false;        // Whether process type classification is available (Windows: App/Background/Windows)
    bool hasGdiObjects = false;         // Whether GDI object count is available (Windows-only via GetGuiResources)
    bool hasReducedPrivileges = false;  // True when the process lacks the privileges to read some data (not
                                        // necessarily curable by elevation: sudo can't restore capabilities a
                                        // container or service dropped).
                                        // Linux: the effective set (CapEff), for root too, lacks CAP_SYS_PTRACE
                                        //        or has neither CAP_DAC_READ_SEARCH nor CAP_DAC_OVERRIDE; when
                                        //        it can't be read, not root. FD counts, I/O and network for other
                                        //        users' processes are then (partly) unavailable (ProcPrivileges.h).
                                        // Windows: non-admin AND EStats was specifically denied (ERROR_ACCESS_DENIED).
                                        //          Remains false when EStats is simply unsupported, because
                                        //          running as Administrator would not restore those counters.
    bool hasSharedMemory = false;       // Whether ProcessCounters::sharedBytes is filled (Linux: statm; not on Windows)
    // How many bits ProcessCounters::pageFaultCount is kept in by the OS before it wraps to 0 (#1184).
    // Linux: 64 (minflt + majflt, unsigned long). Windows: 32 (SYSTEM_PROCESS_INFORMATION's ULONG
    // PageFaultCount, which a long-lived process can pass). Domain takes deltas modulo 2^bits.
    std::uint8_t pageFaultCountBits = 64;
};

} // namespace Platform
