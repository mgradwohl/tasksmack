#pragma once

#include <cstdint>
#include <string>

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

    std::uint64_t startTimeTicks = 0; // For PID reuse detection

    // CPU time (cumulative ticks/jiffies)
    std::uint64_t userTime = 0;
    std::uint64_t systemTime = 0;

    // Memory (bytes)
    std::uint64_t rssBytes = 0;
    std::uint64_t peakRssBytes = 0; // Peak working set (OS-provided on Windows, computed on Linux)
    std::uint64_t virtualBytes = 0;
    std::uint64_t sharedBytes = 0; // Shared memory (from statm on Linux)

    // Optional fields (check capabilities)
    std::uint64_t readBytes = 0;
    std::uint64_t writeBytes = 0;
    std::int32_t threadCount = 0;
    std::uint64_t pageFaultCount = 0;  // Total page faults (minor + major on Linux)
    std::uint64_t cpuAffinityMask = 0; // Bitmask of allowed CPU cores (0 = not available)

    // Network counters (cumulative bytes)
    std::uint64_t netSentBytes = 0;
    std::uint64_t netReceivedBytes = 0;

    // Power usage (optional, platform-dependent)
    // On Windows: from PROCESS_POWER_THROTTLING_STATE
    // On Linux: from powercap sysfs (per-package energy counters)
    std::uint64_t energyMicrojoules = 0; // Cumulative energy consumption in microjoules
};

/// Reports what this platform's probe supports.
/// UI can degrade gracefully for missing capabilities.
struct ProcessCapabilities
{
    bool hasIoCounters = false;
    bool hasThreadCount = false;
    bool hasUserSystemTime = true;
    bool hasStartTime = true;
    bool hasUser = false;            // Whether process owner/user is available
    bool hasCommand = false;         // Whether full command line is available
    bool hasNice = false;            // Whether nice/priority value is available
    bool hasPageFaults = false;      // Whether page fault count is available
    bool hasPeakRss = false;         // Whether peak working set is available
    bool hasCpuAffinity = false;     // Whether CPU affinity mask is available
    bool hasNetworkCounters = false; // Whether per-process network counters are available
    bool hasPowerUsage = false;      // Whether power consumption metrics are available
    bool hasStatus = false;          // Whether process status (Suspended, Efficiency Mode) is available
};

} // namespace Platform
