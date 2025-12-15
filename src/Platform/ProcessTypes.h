#pragma once

#include <cstdint>
#include <string>

namespace Platform
{

/// Raw counters from OS - no computed values.
/// Probes populate this; domain computes deltas and rates.
struct ProcessCounters
{
    int32_t pid = 0;
    int32_t parentPid = 0;
    std::string name;
    char state = '?'; // Raw state character from OS (e.g., 'R', 'S', 'Z')

    uint64_t startTimeTicks = 0; // For PID reuse detection

    // CPU time (cumulative ticks/jiffies)
    uint64_t userTime = 0;
    uint64_t systemTime = 0;

    // Memory (bytes)
    uint64_t rssBytes = 0;
    uint64_t virtualBytes = 0;

    // Optional fields (check capabilities)
    uint64_t readBytes = 0;
    uint64_t writeBytes = 0;
    int32_t threadCount = 0;
};

/// Reports what this platform's probe supports.
/// UI can degrade gracefully for missing capabilities.
struct ProcessCapabilities
{
    bool hasIoCounters = false;
    bool hasThreadCount = false;
    bool hasUserSystemTime = true;
    bool hasStartTime = true;
};

} // namespace Platform
