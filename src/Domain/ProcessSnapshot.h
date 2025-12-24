#pragma once

#include <cstdint>
#include <string>

namespace Domain
{

/// Immutable, UI-ready process data.
/// Computed from raw counter deltas by ProcessModel.
struct ProcessSnapshot
{
    std::int32_t pid = 0;
    std::int32_t parentPid = 0;
    std::string name;
    std::string command;      // Full command line
    std::string user;         // Username (owner) of the process
    std::string displayState; // "Running", "Sleeping", "Zombie", etc.

    double cpuPercent = 0.0;       // Computed from deltas
    double cpuUserPercent = 0.0;   // Computed from deltas (user mode)
    double cpuSystemPercent = 0.0; // Computed from deltas (system/kernel)
    double memoryPercent = 0.0;    // RSS as % of total system memory
    double cpuTimeSeconds = 0.0;   // Cumulative CPU time (user + system)
    std::uint64_t memoryBytes = 0; // RSS
    std::uint64_t peakMemoryBytes = 0; // Peak RSS (from OS on Windows, tracked on Linux)
    std::uint64_t virtualBytes = 0;
    std::uint64_t sharedBytes = 0; // Shared memory
    std::int32_t nice = 0;         // Nice value

    // Optional (0 if not supported)
    double ioReadBytesPerSec = 0.0;
    double ioWriteBytesPerSec = 0.0;
    std::int32_t threadCount = 0;

    /// Stable identity across samples (handles PID reuse).
    /// Computed as hash(pid, startTime).
    std::uint64_t uniqueKey = 0;
};

} // namespace Domain
