#pragma once

#include "Platform/CpuDetails.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{

/// What kind of link a network interface is, as the platform reports it (#2568): Windows from the
/// interface's IANA ifType and NDIS physical medium, Linux from its sysfs ARPHRD type, uevent DEVTYPE
/// and wireless directory. Whether it is virtual is a separate question (InterfaceCounters::isVirtual):
/// a Hyper-V vEthernet adapter is an Ethernet link that is virtual.
enum class NetInterfaceKind : std::uint8_t
{
    Unknown,   ///< The platform couldn't tell (no sysfs entry, a synthetic interface): the UI goes by its name.
    Ethernet,  ///< Wired Ethernet, or anything that presents itself as Ethernet (vEthernet, TAP, veth).
    WiFi,      ///< 802.11 Wi-Fi.
    Bluetooth, ///< A Bluetooth PAN link (Windows' Bluetooth medium, Linux's bnep).
    Cellular,  ///< Mobile broadband (WWAN): Windows' WWAN ifTypes, Linux's wwan DEVTYPE or raw-IP modems.
    Loopback,  ///< The loopback interface.
    Tunnel,    ///< A tunnel or point-to-point link: VPN, PPP, GRE, SIT, IP-in-IP, tun, WireGuard.
    Other,     ///< Classified, but none of the above (InfiniBand, FireWire, CAN, ...).
};

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

    // Compressed swap (#3087), Linux. zram: every /sys/block/zramN/mm_stat, summed. zswap: /proc/meminfo's
    // Zswapped (pages held, before compression) and Zswap (the pool's compressed size), kernel 5.19+.
    // hasZram: at least one zram device was read this sample. hasZswap: zswap holds something.
    bool hasZram = false;
    uint64_t zramOriginalBytes = 0;   ///< Data stored in zram, before compression
    uint64_t zramCompressedBytes = 0; ///< Its compressed size
    uint64_t zramMemoryUsedBytes = 0; ///< RAM the devices take: compressed data plus allocator overhead
    bool hasZswap = false;
    uint64_t zswapOriginalBytes = 0;   ///< Zswapped
    uint64_t zswapCompressedBytes = 0; ///< Zswap
};

/// Combined system counters snapshot.
/// One line of a Linux pressure stall information file (#3062): the share of wall time, in percent, that
/// tasks were stalled on the resource, averaged over the last 10, 60 and 300 seconds.
struct PressureStall
{
    double avg10 = 0.0;
    double avg60 = 0.0;
    double avg300 = 0.0;
};

/// A resource's /proc/pressure/<resource> (#3062). `some`: at least one task was stalled. `full`: every
/// non-idle task was stalled at once (CPU reports it from Linux 5.13, as zeros). Either is nullopt when
/// the file didn't have that line.
struct ResourcePressure
{
    std::optional<PressureStall> some;
    std::optional<PressureStall> full;

    [[nodiscard]] bool read() const noexcept
    {
        return some.has_value() || full.has_value();
    }
};

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

    // Pressure stall information (Linux PSI, #3062), where SystemCapabilities::hasPressure says it's read.
    ResourcePressure cpuPressure;
    ResourcePressure memoryPressure;
    ResourcePressure ioPressure;

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
        // The link's kind, for the Interface Status table's Type icon (#2568).
        NetInterfaceKind kind = NetInterfaceKind::Unknown;
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
    bool hasPressure = false; // /proc/pressure: Linux PSI, when the kernel has it enabled (#3062)
    bool hasCpuFreq = false;
    bool hasNetworkCounters = false; // System-wide network byte counters
    // CpuDetails' virtualization fields (firmware, SLAT, hypervisor) mean something here (Windows,
    // and Linux since #2592). Elsewhere the CPU Details block leaves those rows out rather than
    // showing them as unknown (#809).
    bool hasVirtualizationInfo = false;
    // CpuDetails' virtualization-based security and memory integrity fields mean something here
    // (Windows only, #809); without it those two rows are left out (#2592).
    bool hasVirtualizationSecurityInfo = false;
};

} // namespace Platform
