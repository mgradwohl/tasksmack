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
};

/// Reports what this platform's probe supports.
/// UI can degrade gracefully for missing capabilities.
struct ProcessCapabilities
{
    bool hasIoCounters = false;
    bool hasThreadCount = false;
    bool hasUserSystemTime = true;
    bool hasStartTime = true;
    bool hasUser = false;      // Whether process owner/user is available
    bool hasCommand = false;   // Whether full command line is available
    bool hasNice = false;      // Whether nice/priority value is available
    bool hasPeakRss = false;   // Whether peak working set is available
};

} // namespace Platform
