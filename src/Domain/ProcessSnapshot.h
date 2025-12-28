#pragma once

#include <cstdint>
#include <string>

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
    std::int32_t nice = 0;         // Nice value
    std::int32_t threadCount = 0;  // Optional (0 if not supported)
    
    double cpuPercent = 0.0;           // Computed from deltas
    double memoryPercent = 0.0;        // RSS as % of total system memory
    double cpuTimeSeconds = 0.0;       // Cumulative CPU time (user + system)
    
    std::uint64_t memoryBytes = 0;     // RSS
    std::uint64_t virtualBytes = 0;
    std::uint64_t uniqueKey = 0;       // Stable identity across samples (hash(pid, startTime))
    
    // Less frequently accessed metrics
    double cpuUserPercent = 0.0;       // Computed from deltas (user mode)
    double cpuSystemPercent = 0.0;     // Computed from deltas (system/kernel)
    double ioReadBytesPerSec = 0.0;    // Optional (0 if not supported)
    double ioWriteBytesPerSec = 0.0;   // Optional (0 if not supported)
    double netSentBytesPerSec = 0.0;   // Optional (0 if not supported)
    double netReceivedBytesPerSec = 0.0; // Optional (0 if not supported)
    
    std::uint64_t peakMemoryBytes = 0; // Peak RSS (from OS on Windows, tracked on Linux)
    std::uint64_t sharedBytes = 0;     // Shared memory
    std::uint64_t pageFaults = 0;      // Total page faults (cumulative)
    std::uint64_t cpuAffinityMask = 0; // Bitmask of allowed CPU cores (0 = not available)
    
    // Strings at the end (reduce padding and improve cache for hot integer/float fields)
    std::string name;
    std::string command;      // Full command line
    std::string user;         // Username (owner) of the process
    std::string displayState; // "Running", "Sleeping", "Zombie", etc.
};

} // namespace Domain
