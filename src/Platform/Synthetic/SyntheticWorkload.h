#pragma once

// A deterministic synthetic machine for limit-setting captures (#1413): thousands of processes in a
// realistic tree, many cores, disks and interfaces, every counter a pure function of time.
//
// Every cumulative counter is the closed-form integral of a smooth, positive rate (a base times one
// plus two sines), evaluated at the requested time. So a reading can be taken at any time, in any
// order -- the live probes read "now", while the App's history preload reads 18k past instants --
// and successive readings always give consistent deltas. The same seed gives the same machine, and
// the same time the same counters, on every platform: the generator uses its own SplitMix64 rather
// than <random>'s distributions, whose output differs between standard libraries.
//
// Not OS code, but it implements the Platform probe interfaces' raw-counter contract, so it lives
// beside the real probes; only the App composition root constructs it, and only when the
// TASKSMACK_SYNTHETIC environment variable asks for it.

#include "Platform/ProcessTypes.h"
#include "Platform/StorageTypes.h"
#include "Platform/SystemTypes.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Platform::Synthetic
{

/// What the synthetic machine has. The App parses it from TASKSMACK_SYNTHETIC and clamps every
/// count to the bounds below.
struct WorkloadSpec
{
    std::size_t processes = 2000;
    std::size_t cores = 16;
    std::size_t disks = 4;
    std::size_t interfaces = 4;
    std::uint64_t seed = 1413;
};

inline constexpr std::size_t MIN_PROCESSES = 1;
inline constexpr std::size_t MAX_PROCESSES = 100'000;
inline constexpr std::size_t MIN_CORES = 1;
inline constexpr std::size_t MAX_CORES = 1024;
inline constexpr std::size_t MAX_DISKS = 64;
inline constexpr std::size_t MAX_INTERFACES = 256;

/// The per-process totals ProcessModel aggregates into its system histories (threads, page faults,
/// network, power), as instantaneous rates: what the App preloads those histories with.
struct ProcessTotals
{
    double netSentBytesPerSec = 0.0;
    double netReceivedBytesPerSec = 0.0;
    double pageFaultsPerSec = 0.0;
    double threadCount = 0.0;
    double handleCount = 0.0;
    double powerWatts = 0.0;
};

class Workload
{
  public:
    /// CPU time unit of every tick counter (process and system): microseconds, so per-process CPU%
    /// moves smoothly instead of in USER_HZ-sized steps.
    static constexpr long TICKS_PER_SECOND = 1'000'000;
    /// The machine's uptime at @p epoch: three days, so every long-lived process started well before
    /// the longest history window reaches back.
    static constexpr double UPTIME_AT_EPOCH_SECONDS = 3.0 * 24.0 * 3600.0;
    /// A busy core's share of package power, for the per-process energy counters.
    static constexpr double WATTS_PER_BUSY_CORE = 4.0;

    /// @param epoch          steady_clock instant at which the machine's uptime is UPTIME_AT_EPOCH_SECONDS.
    /// @param bootUnixSeconds Wall-clock boot time, for process start times and SystemCounters::bootTimestamp.
    Workload(const WorkloadSpec& spec, std::chrono::steady_clock::time_point epoch, std::uint64_t bootUnixSeconds);
    /// Boots the machine UPTIME_AT_EPOCH_SECONDS before now.
    explicit Workload(const WorkloadSpec& spec);

    [[nodiscard]] const WorkloadSpec& spec() const noexcept
    {
        return m_Spec;
    }

    /// Machine uptime (seconds) at a steady_clock instant: the time every *At() reading takes.
    [[nodiscard]] double uptimeAt(std::chrono::steady_clock::time_point time) const noexcept;

    /// Every process alive at @p uptime, in a stable order (parents before children). Reuses @p out.
    void processesAt(double uptime, std::vector<ProcessCounters>& out) const;
    /// Total CPU time of all cores, all states (IProcessProbe::totalCpuTime()), at @p uptime.
    [[nodiscard]] std::uint64_t totalCpuTicksAt(double uptime) const noexcept;
    [[nodiscard]] std::uint64_t totalMemoryBytes() const noexcept
    {
        return m_TotalMemoryBytes;
    }
    /// The per-process totals at @p uptime, summed in closed form (O(1) in the process count).
    [[nodiscard]] ProcessTotals processTotalsAt(double uptime) const noexcept;

    /// System counters at @p uptime. Reuses @p out's vectors.
    void systemCountersAt(double uptime, SystemCounters& out) const;
    /// Disk counters at @p uptime. Reuses @p out's vectors.
    void diskCountersAt(double uptime, SystemDiskCounters& out) const;

    [[nodiscard]] static ProcessCapabilities processCapabilities() noexcept;
    [[nodiscard]] static SystemCapabilities systemCapabilities() noexcept;
    [[nodiscard]] static DiskCapabilities diskCapabilities() noexcept;

    /// A positive rate, base * (1 + a1 sin(w1 t + p1) + a2 sin(w2 t + p2)) with a1 + a2 < 1, and its
    /// integral from 0. Each sine's frequency is one of BAND_COUNT shared bands, so a sum of many
    /// waves collapses to a few sines per band (processTotalsAt()).
    struct Harmonic
    {
        double amplitude = 0.0;
        double phase = 0.0;
        std::size_t band = 0;
    };
    struct Wave
    {
        double base = 0.0;
        std::array<Harmonic, 2> harmonics{};

        [[nodiscard]] double rate(double t) const noexcept;
        [[nodiscard]] double integral(double t) const noexcept;
    };
    static constexpr std::size_t BAND_COUNT = 8;

  private:
    /// One process slot. A long-lived slot holds one process for the machine's whole life; a churning
    /// slot (lifetime > 0) holds a succession of processes, each replaced by a new PID when it ends,
    /// so the process count stays fixed while processes start and exit.
    struct Slot
    {
        std::int32_t pid = 0;
        std::int32_t parentPid = 0;
        std::string name;
        std::string command;
        std::string user;
        std::int32_t nice = 0;
        bool kernelThread = false;
        double startSeconds = 0.0;    // long-lived slots
        double lifetimeSeconds = 0.0; // churning slots: each process's lifetime (0 = long-lived)
        double lifetimeOffset = 0.0;  // churning slots: where in its lifetime the slot is at uptime 0
        std::size_t churnIndex = 0;   // churning slots: PID block
        Wave cpu;                     // cores
        Wave readBytes;               // bytes/s
        Wave writeBytes;              // bytes/s
        Wave netSent;                 // bytes/s
        Wave netReceived;             // bytes/s
        Wave pageFaults;              // faults/s
        double rssBytes = 0.0;
        double rssPhase = 0.0;
        std::int32_t threads = 1;
        std::int32_t handles = 0;
    };

    /// Per-band sums of base * amplitude * {cos, sin}(phase) over every slot, plus the sum of the
    /// bases: enough to evaluate the sum of all slots' rates at any time in O(BAND_COUNT).
    struct WaveSum
    {
        double base = 0.0;
        std::array<double, BAND_COUNT> sinCoefficient{};
        std::array<double, BAND_COUNT> cosCoefficient{};

        void add(const Wave& wave) noexcept;
        [[nodiscard]] double at(double t) const noexcept;
        /// The integral of at() from 0 to @p t.
        [[nodiscard]] double integral(double t) const noexcept;
    };

    /// A core's busy time is its share of every process's CPU work plus its own overhead (kernel,
    /// interrupts), so the system's busy time always covers all the processes' work (#1413 review).
    struct CoreWaves
    {
        double processShare = 0.0; // fraction of all process work this core runs; the shares sum to 1
        Wave overhead;             // fraction of the core, on top of its process work
        Wave iowait;               // fraction of the core
    };
    struct InterfaceSpec
    {
        std::string name;
        bool isVirtual = false;
        std::uint64_t linkSpeedMbps = 0;
        Wave rx; // bytes/s
        Wave tx; // bytes/s
    };
    struct DiskSpec
    {
        std::string name;
        Wave readBytes;  // bytes/s
        Wave writeBytes; // bytes/s
        Wave busy;       // fraction of time active
    };

    void buildProcesses();
    void buildSystem();

    WorkloadSpec m_Spec;
    std::chrono::steady_clock::time_point m_Epoch;
    std::uint64_t m_BootUnixSeconds = 0;
    std::uint64_t m_TotalMemoryBytes = 0;
    double m_ProcessRssBytes = 0.0;

    std::vector<Slot> m_Slots;
    WaveSum m_CpuSum;
    WaveSum m_NetSentSum;
    WaveSum m_NetReceivedSum;
    WaveSum m_PageFaultSum;
    double m_ThreadSum = 0.0;
    double m_HandleSum = 0.0;

    std::vector<CoreWaves> m_Cores;
    std::vector<InterfaceSpec> m_Interfaces;
    std::vector<DiskSpec> m_Disks;
    Wave m_MemoryWave;
};

} // namespace Platform::Synthetic
