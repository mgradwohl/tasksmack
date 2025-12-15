#pragma once

#include <cstdint>
#include <string>

namespace Domain
{

/// Immutable, UI-ready process data.
/// Computed from raw counter deltas by ProcessModel.
struct ProcessSnapshot
{
    int32_t pid = 0;
    int32_t parentPid = 0;
    std::string name;
    std::string displayState; // "Running", "Sleeping", "Zombie", etc.

    double cpuPercent = 0.0;  // Computed from deltas
    uint64_t memoryBytes = 0; // RSS
    uint64_t virtualBytes = 0;

    // Optional (0 if not supported)
    double ioReadBytesPerSec = 0.0;
    double ioWriteBytesPerSec = 0.0;
    int32_t threadCount = 0;

    /// Stable identity across samples (handles PID reuse).
    /// Computed as hash(pid, startTime).
    uint64_t uniqueKey = 0;
};

} // namespace Domain
