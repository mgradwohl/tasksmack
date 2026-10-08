#pragma once

#include "Platform/CpuAffinity.h"
#include "PriorityConfig.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Domain
{

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
    std::uint64_t uniqueKey = 0;      // Stable identity across samples (hash(pid, startTime))

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
    std::string displayState; // "Running", "Sleeping", "Zombie", etc.
    std::string status;       // Process status (e.g., "Suspended", "Efficiency Mode")
    std::string gpuDevices;   // Comma-separated GPU IDs: "0" or "0,1"
    std::string publisher;    // Software publisher/vendor (Windows PE version info; empty if not available)
    std::string processType;  // Process type: "App", "Background Process", "Windows Process" (Windows-only)

    // Tree hierarchy (computed once per snapshot)
    std::vector<std::size_t> childrenIndices; // Indices of children within the same snapshot vector

    // GPU engines (union of active engines across all GPUs)
    std::vector<std::string> gpuEngines; // ["3D", "Compute"]

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
    /// The GPU probe supported per-process data, but reading it failed for that generation: its GPU
    /// fields are a gap, not a measurement (#1210).
    bool gpuReadFailed = false;
};

} // namespace Domain
