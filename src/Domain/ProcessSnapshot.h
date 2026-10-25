#pragma once

#include "Platform/CpuAffinity.h"
#include "PriorityConfig.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Domain
{

/// An immutable list of names that copies of a snapshot share (#2043). ProcessModel hands out its
/// snapshots by value every frame, and a std::vector<std::string> member made every copy allocate --
/// on Linux nearly every process has a systemd unit, so nearly every snapshot carried one -- which made
/// those copies ~50% slower. Copying this copies a pointer. Reads like a const vector.
class SharedNames
{
  public:
    SharedNames() = default;
    // Implicit on purpose: `snapshot.serviceNames = names;` and `= {"Dhcp"}` read as for the vector it replaces.
    SharedNames(std::vector<std::string> names) // NOLINT(cppcoreguidelines-explicit-constructor,misc-explicit-constructor)
        : m_Names(names.empty() ? nullptr : std::make_shared<const std::vector<std::string>>(std::move(names)))
    {}
    SharedNames(std::initializer_list<std::string> names) // NOLINT(cppcoreguidelines-explicit-constructor,misc-explicit-constructor)
        : SharedNames(std::vector<std::string>(names))
    {}

    [[nodiscard]] bool empty() const noexcept
    {
        return m_Names == nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Names == nullptr ? 0 : m_Names->size();
    }
    [[nodiscard]] const std::string& operator[](std::size_t index) const
    {
        return (*m_Names)[index];
    }
    [[nodiscard]] std::span<const std::string> span() const noexcept
    {
        return m_Names == nullptr ? std::span<const std::string>{} : std::span<const std::string>(*m_Names);
    }
    [[nodiscard]] auto begin() const noexcept
    {
        return span().begin();
    }
    [[nodiscard]] auto end() const noexcept
    {
        return span().end();
    }

  private:
    std::shared_ptr<const std::vector<std::string>> m_Names; // null: none
};

/// Immutable, UI-ready process data.
/// Computed from raw counter deltas by ProcessModel.
/// Fields are ordered to optimize cache locality and minimize padding.
struct ProcessSnapshot
{
    // Hot data (frequently accessed during table rendering)
    std::int32_t pid = 0;
    std::int32_t parentPid = 0;
    std::int32_t nice = 0;        // Nice value
    std::int32_t threadCount = 0; // Optional (0 if not supported)
    std::int32_t handleCount = 0; // Handle count (Windows) / FD count (Linux)
    // The platform's priority class (Windows), which names the priority where nice can't (Realtime
    // and High share a nice bucket, #1280); None on Linux. See Priority::getProcessPriorityLabel().
    Priority::PriorityClass priorityClass = Priority::PriorityClass::None;

    double cpuPercent = 0.0;     // Computed from deltas
    double memoryPercent = 0.0;  // RSS as % of total system memory
    double cpuTimeSeconds = 0.0; // Cumulative CPU time (user + system)

    std::uint64_t memoryBytes = 0; // RSS
    std::uint64_t virtualBytes = 0;
    std::uint64_t startTimeEpoch = 0; // Process start time (Unix epoch seconds)
    std::uint64_t startTimeTicks = 0; // Raw platform start time; with pid, what process actions verify (#973)
    std::uint64_t uniqueKey = 0;      // Stable across samples: a 64-bit mix of (pid, startTimeTicks), not collision-free (#2115)

    // Less frequently accessed metrics
    double cpuUserPercent = 0.0;         // Computed from deltas (user mode)
    double cpuSystemPercent = 0.0;       // Computed from deltas (system/kernel)
    double ioReadBytesPerSec = 0.0;      // Optional (0 if not supported)
    double ioWriteBytesPerSec = 0.0;     // Optional (0 if not supported)
    double netSentBytesPerSec = 0.0;     // Optional (0 if not supported)
    double netReceivedBytesPerSec = 0.0; // Optional (0 if not supported)
    double pageFaultsPerSec = 0.0;       // Optional (0 if not supported)
    double powerWatts = 0.0;             // Current power consumption in watts (computed from energy delta)

    std::uint64_t peakMemoryBytes = 0; // Peak RSS (from OS on Windows, tracked on Linux)
    std::uint64_t sharedBytes = 0;     // Shared memory
    std::uint64_t privateBytes = 0;    // Private memory (Linux: RssAnon, #1885)
    std::uint64_t swapBytes = 0;       // Swapped out (Linux: VmSwap, #1885)
    // PSS, USS and swap PSS (Linux: smaps_rollup, #1885): costly, so read only for ProcessModel's
    // watched process, in its watched sample (memoryRollupRead); zero everywhere else.
    std::uint64_t pssBytes = 0;
    std::uint64_t ussBytes = 0;
    std::uint64_t swapPssBytes = 0;
    bool memoryRollupRead = false;
    bool memoryRollupDenied = false;   // The OS refused it (another user's process, #2309): "not readable", not just absent
    std::uint64_t pageFaults = 0;      // Total page faults (cumulative)
    Platform::CpuAffinity cpuAffinity; // Logical processors it may run on (empty = not available)

    // GPU usage (per-process, across the GPUs it uses), on the same terms as the adapter figures on
    // the GPU tab (#1164). Utilization is that of the busiest GPU it uses, 0-100, as an adapter's is
    // (not a sum, which could pass 100%). "GPU memory" counts on each GPU the memory the GPU tab
    // reports as used for it -- shared (system) memory on an integrated GPU where the platform
    // reports it (Windows), otherwise dedicated memory -- summed across GPUs, so it never exceeds
    // what the adapters show in use. The
    // dedicated and shared amounts are also kept apart.
    double gpuUtilPercent = 0.0;               // Busiest GPU's utilization by this process, 0-100
    std::uint64_t gpuMemoryBytes = 0;          // As each GPU's "used" figure counts it, summed across GPUs
    std::uint64_t gpuDedicatedMemoryBytes = 0; // Dedicated (VRAM) across all GPUs
    std::uint64_t gpuSharedMemoryBytes = 0;    // Shared (system memory mapped by the GPU) across all GPUs
    double gpuEncoderUtil = 0.0;               // Busiest GPU's encoder utilization
    double gpuDecoderUtil = 0.0;               // Busiest GPU's decoder utilization

    // NPU usage (#1799): the process's use of the NPUs and other compute accelerators (Windows MCDM
    // adapters), on the same terms as its GPU usage above but never counted in it. Read with the GPU
    // fields (gpuFieldsRead); shown in the NPU columns and Process Details' NPU Usage card.
    double npuUtilPercent = 0.0;               // Busiest accelerator's utilization by this process, 0-100
    std::uint64_t npuMemoryBytes = 0;          // As each accelerator's "used" figure counts it, summed
    std::uint64_t npuDedicatedMemoryBytes = 0; // Dedicated memory across all accelerators
    std::uint64_t npuSharedMemoryBytes = 0;    // Shared (system) memory across all accelerators

    // GDI object count (optional, Windows-only via GetGuiResources).
    // std::nullopt means the probe could not open the process with the required rights.
    // A stored value of 0 means the process is accessible but owns no GDI objects.
    std::optional<std::int32_t> gdiObjectCount;

    // Whether this snapshot's GPU fields were read by a per-process GPU read (#1210). False for a
    // process first listed since the GPU sampler's last read, which therefore could not have seen it
    // (#1417), and for every process before the first read: its GPU fields are defaults, never read,
    // and must not be shown as measured zeros.
    bool gpuFieldsRead = true;

    // Whether a value was read for this process (#1110). False: the probe could not read it --
    // typically for lack of rights, e.g. another user's process without root on Linux -- and the
    // value is a placeholder 0, to be shown as unavailable and left out of totals, never as a
    // measured 0. A rate is available only when both readings it is taken between were.
    bool handleCountAvailable = true; // handleCount
    bool ioAvailable = true;          // ioReadBytesPerSec / ioWriteBytesPerSec
    bool networkAvailable = true;     // netSentBytesPerSec / netReceivedBytesPerSec

    // Strings at the end (reduce padding and improve cache for hot integer/float fields)
    std::string name;
    std::string command;      // Full command line
    std::string user;         // Username (owner) of the process
    std::string realUser;     // Who started it, when not `user` (a setuid program's caller, #2475); else empty
    std::string displayState; // "Running", "Sleeping", "Zombie", etc.
    std::string status;       // Process status (e.g., "Suspended", "Efficiency Mode")
    std::string gpuDevices;   // Comma-separated GPU IDs: "0" or "0,1"
    std::string publisher;    // Software publisher/vendor (Windows PE version info; empty if not available)
    std::string container;    // Container: "docker:1a2b3c4d5e6f", "lxc:web" (Linux cgroups; empty if in none)
    std::string services;     // Services it runs, comma-joined: "sshd.service", "Dhcp, EventLog" (#1886, #1880)
    std::string processType;  // Process type: "App", "Background Process", "Windows Process" (Windows-only)
    std::string architecture; // Architecture it runs as: "x64", "ARM64", "x64 (emulated)", "x86 (WOW64)" (#2394; empty if unread)

    // The services the process runs, by name (#1880): Windows, those the service list gives its PID (in
    // the list's name order); Linux, the systemd unit its cgroup names (#1886). Empty for none, or on
    // Windows while no current service list is attached (ProcessServices::mergeInto()). `services` above is
    // them joined with ", ", as the Services column shows and sorts them.
    SharedNames serviceNames;

    // Tree hierarchy (computed once per snapshot)
    std::vector<std::size_t> childrenIndices; // Indices of children within the same snapshot vector

    // GPU engines (union of active engines across all GPUs)
    std::vector<std::string> gpuEngines; // ["3D", "Compute"]

    // NPU engines (union of active engines across the NPUs and compute accelerators, #1799) and their
    // names, comma-separated, as gpuEngines and gpuDevices are for the GPUs
    std::vector<std::string> npuEngines; // ["Compute"]
    std::string npuDevices;              // "Intel(R) AI Boost"

    // Per-GPU breakdown (for tooltip/details view)
    struct PerGPUUsage
    {
        std::string gpuId;                      // GPU identifier
        std::string gpuName;                    // e.g., "NVIDIA RTX 4090"
        bool isIntegrated = false;              // Integrated vs discrete
        double utilPercent = 0.0;               // GPU % on this specific GPU, 0-100
        std::uint64_t memoryBytes = 0;          // As this GPU's "used" figure counts it (#1164)
        std::uint64_t dedicatedMemoryBytes = 0; // VRAM allocated on this GPU
        std::uint64_t sharedMemoryBytes = 0;    // System memory this GPU maps for the process
        std::vector<std::string> engines;       // Active engines on this GPU
    };
    std::vector<PerGPUUsage> perGpuUsage; // Breakdown for multi-GPU processes
};

/// One process as one published generation saw it (ProcessModel::watchedSamplesSince(), #1098).
struct ProcessSample
{
    /// The process in that generation, or nullptr when the generation did not list it (it exited,
    /// or the watched PID is not running). Immutable and shared, so handing it on copies nothing.
    std::shared_ptr<const ProcessSnapshot> snapshot;
    /// The generation's ProcessModel::snapshotVersion(). Every generation published while a process
    /// is watched gets a sample, so consecutive samples have consecutive versions.
    std::uint64_t version = 0;
    /// When that generation was sampled, as std::chrono::steady_clock seconds since its epoch -- the
    /// timebase of ProcessSystemHistories::timestamps -- not when a reader happened to see it.
    double sampleTimeSeconds = 0.0;
    /// Whether the probe could supply per-process I/O and network counters at all when that generation
    /// was published (Platform::ProcessCapabilities::hasIoCounters / hasNetworkCounters, as published
    /// with it). A probe can withdraw one between generations (#1254), so a reader judges each sample by
    /// its own generation's state, not the latest: a reading taken while it was supported stays one.
    bool ioCountersSupported = true;
    bool networkCountersSupported = true;
    /// Likewise for the GPU probe when that generation was produced (#1210): whether it supplied
    /// per-process GPU data at all, and per-process utilization among it. The GPU model can gain or
    /// lose either on re-enumeration, on its own sampler, so each sample carries its own.
    bool gpuPerProcessSupported = true;
    bool gpuUtilizationSupported = true;
    /// The same for per-process NPU / compute-accelerator data, apart from the GPU's (#1827).
    bool npuPerProcessSupported = true;
    bool npuUtilizationSupported = true;
    /// The GPU probe supported per-process data, but reading it failed for that generation: its GPU
    /// fields are a gap, not a measurement (#1210).
    bool gpuReadFailed = false;
};

} // namespace Domain
