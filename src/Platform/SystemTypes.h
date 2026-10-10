#pragma once

#include "Platform/CpuDetails.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

/// Raw CPU counters from OS (cumulative ticks/jiffies).
/// Probes populate this; domain computes deltas and percentages.
struct CpuCounters
{
    uint64_t user = 0;      // Normal processes executing in user mode (Linux: includes guest)
    uint64_t nice = 0;      // Niced processes executing in user mode (Linux: includes guest_nice)
    uint64_t system = 0;    // Processes executing in kernel mode
    uint64_t idle = 0;      // Twiddling thumbs
    uint64_t iowait = 0;    // Idle while waiting for I/O to complete
    uint64_t irq = 0;       // Servicing interrupts
    uint64_t softirq = 0;   // Servicing softirqs
    uint64_t steal = 0;     // Involuntary wait (virtualized)
    uint64_t guest = 0;     // Running a guest (virtualized); already counted in user, informational only
    uint64_t guestNice = 0; // Running a niced guest; already counted in nice, informational only

    /// Stable identity of a per-core entry: the OS's logical CPU number (Linux `cpuN`, Windows
    /// processor index). Unused on SystemCounters::cpuTotal. The Linux kernel lists only online
    /// CPUs, so an offline interior CPU leaves a hole in the ids rather than shifting every later
    /// core down a position; consumers match samples and history by this id, never by vector
    /// position (#1229).
    std::size_t coreId = 0;

    /// Total CPU time (all states). guest and guestNice are left out: the Linux kernel already
    /// adds guest time to user and nice, so counting them again overstated a VM host's load
    /// (#1157).
    [[nodiscard]] uint64_t total() const
    {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }

    /// Time spent idle: idle plus iowait. iowait is a CPU with nothing to run while it waits for
    /// I/O; it is reported on its own as a breakdown, but it is not busy time. Windows has no
    /// iowait and counts that time as idle, so this keeps "busy" the same on both platforms (#1157).
    [[nodiscard]] uint64_t idleTotal() const
    {
        return idle + iowait;
    }

    /// Active (busy) time: total() minus idleTotal(), each tick counted once.
    [[nodiscard]] uint64_t active() const
    {
        return user + nice + system + irq + softirq + steal;
    }
};

/// Raw memory counters from OS (in bytes or pages, converted to bytes).
struct MemoryCounters
{
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t availableBytes = 0; // Available for starting new apps (includes cached)
    // Whether availableBytes is a real reading, set by each probe once it has one. Linux kernels
    // before 3.14 have no MemAvailable, and a failed read has nothing; a reading of 0 is real
    // (memory exhausted), not "missing" (#1143).
    bool hasAvailableBytes = false;
    uint64_t buffersBytes = 0;
    uint64_t cachedBytes = 0;

    uint64_t swapTotalBytes = 0;
    uint64_t swapFreeBytes = 0;

    // Commit charge (#1627): the virtual memory the system has promised (Windows CommitTotal, Linux
    // Committed_AS) and the most it may promise (CommitLimit), in bytes. hasCommitCharge is set once
    // both were read this sample. commitPeakBytes is 0 where the platform keeps no peak (Linux).
    bool hasCommitCharge = false;
    uint64_t commitChargeBytes = 0;
    uint64_t commitLimitBytes = 0;
    uint64_t commitPeakBytes = 0;
};

/// Combined system counters snapshot.
struct SystemCounters
{
    CpuCounters cpuTotal;                // Aggregate across all cores
    std::vector<CpuCounters> cpuPerCore; // Per-core (optional); online cores only, each tagged with its coreId
    MemoryCounters memory;

    uint64_t uptimeSeconds = 0;
    uint64_t bootTimestamp = 0; // Unix epoch

    // Load average (1, 5, 15 minute)
    double loadAvg1 = 0.0;
    double loadAvg5 = 0.0;
    double loadAvg15 = 0.0;

    // The current CPU clock in MHz (#1184). Linux: cpu0's cpufreq reading. Windows: the base clock (~MHz)
    // scaled by "% Processor Performance", the processors' average, as Task Manager's "Speed".
    uint64_t cpuFreqMHz = 0;

    // Network counters (cumulative bytes across the interfaces counted in the Total; see
    // InterfaceCounters::isVirtual)
    uint64_t netRxBytes = 0; // Total bytes received
    uint64_t netTxBytes = 0; // Total bytes transmitted

    // Per-interface network counters
    struct InterfaceCounters
    {
        std::string name;           // System name: "eth0", "Ethernet", etc.
        std::string displayName;    // Friendly name for UI (may be same as name)
        uint64_t rxBytes = 0;       // Cumulative bytes received
        uint64_t txBytes = 0;       // Cumulative bytes transmitted
        bool isUp = false;          // Interface operational status
        uint64_t linkSpeedMbps = 0; // Link speed in Mbps (0 if unknown)
        // Software interface (bridge, veth, tunnel/VPN, VLAN, loopback-like) whose traffic also crosses
        // a hardware interface, so the network Total leaves it out unless no hardware interface is
        // listed (#1106). Linux: no /sys/class/net/<if>/device. Windows: HardwareInterface clear (#1257),
        // except a Bluetooth PAN link (#1284).
        bool isVirtual = false;
        // Whether the probe could classify the interface, so isVirtual is its answer (false: it
        // couldn't, e.g. no sysfs entry, and isVirtual is false by default). The UI falls back to a
        // name heuristic only then (#1260).
        bool isVirtualKnown = false;
    };
    std::vector<InterfaceCounters> networkInterfaces;

    // Static system info (populated once)
    std::string hostname;
    std::string cpuModel;
    std::size_t cpuCoreCount = 0;
    // Topology, base clock, caches and virtualization status (#809): cached by the probe, which
    // re-reads them when the set of active processors changes (CpuDetails.h)
    CpuDetails cpuDetails;
};

/// Reports what this platform's system probe supports.
struct SystemCapabilities
{
    bool hasPerCoreCpu = false;
    bool hasMemoryAvailable = false; // Some older kernels lack MemAvailable
    bool hasSwap = false;
    bool hasCommitCharge = false; // MemoryCounters' commit charge and limit (#1627)
    bool hasUptime = false;
    bool hasIoWait = false;
    bool hasSteal = false;
    bool hasLoadAvg = false;
    bool hasCpuFreq = false;
    bool hasNetworkCounters = false; // System-wide network byte counters
    // CpuDetails' virtualization fields mean something here (Windows). Elsewhere the CPU Details
    // block leaves those rows out rather than showing them as unknown (#809).
    bool hasVirtualizationInfo = false;
};

} // namespace Platform
