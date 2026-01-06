#pragma once

#include <cstdint>
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

    double cpuPercent = 0.0;     // Computed from deltas
    double memoryPercent = 0.0;  // RSS as % of total system memory
    double cpuTimeSeconds = 0.0; // Cumulative CPU time (user + system)

    std::uint64_t memoryBytes = 0; // RSS
    std::uint64_t virtualBytes = 0;
    std::uint64_t startTimeEpoch = 0; // Process start time (Unix epoch seconds)
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
    std::uint64_t cpuAffinityMask = 0; // Bitmask of allowed CPU cores (0 = not available)

    // GPU usage (per-process, aggregated across all GPUs)
    double gpuUtilPercent = 0.0;      // Total GPU % across all GPUs process uses
    std::uint64_t gpuMemoryBytes = 0; // Total VRAM allocated across all GPUs
    double gpuEncoderUtil = 0.0;      // Aggregate encoder utilization
    double gpuDecoderUtil = 0.0;      // Aggregate decoder utilization

    // Strings at the end (reduce padding and improve cache for hot integer/float fields)
    std::string name;
    std::string command;      // Full command line
    std::string user;         // Username (owner) of the process
    std::string displayState; // "Running", "Sleeping", "Zombie", etc.
    std::string status;       // Process status (e.g., "Suspended", "Efficiency Mode")
    std::string gpuDevices;   // Comma-separated GPU IDs: "0" or "0,1"

    // GPU engines (union of active engines across all GPUs)
    std::vector<std::string> gpuEngines; // ["3D", "Compute"]

    // Per-GPU breakdown (for tooltip/details view)
    struct PerGPUUsage
    {
        std::string gpuId;                // GPU identifier
        std::string gpuName;              // e.g., "NVIDIA RTX 4090"
        bool isIntegrated = false;        // Integrated vs discrete
        double utilPercent = 0.0;         // GPU % on this specific GPU
        std::uint64_t memoryBytes = 0;    // VRAM allocated on this GPU
        std::vector<std::string> engines; // Active engines on this GPU
    };
    std::vector<PerGPUUsage> perGpuUsage; // Breakdown for multi-GPU processes
};

} // namespace Domain
