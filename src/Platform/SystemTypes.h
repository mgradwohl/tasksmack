#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

/// Raw CPU counters from OS (cumulative ticks/jiffies).
/// Probes populate this; domain computes deltas and percentages.
struct CpuCounters
{
    uint64_t user = 0;    // Normal processes executing in user mode
    uint64_t nice = 0;    // Niced processes executing in user mode
    uint64_t system = 0;  // Processes executing in kernel mode
    uint64_t idle = 0;    // Twiddling thumbs
    uint64_t iowait = 0;  // Waiting for I/O to complete
    uint64_t irq = 0;     // Servicing interrupts
    uint64_t softirq = 0; // Servicing softirqs
    uint64_t steal = 0;   // Involuntary wait (virtualized)
    uint64_t guest = 0;   // Running a guest (virtualized)
    uint64_t guestNice = 0;

    /// Total CPU time (all states).
    [[nodiscard]] uint64_t total() const
    {
        return user + nice + system + idle + iowait + irq + softirq + steal + guest + guestNice;
    }

    /// Active (non-idle) time.
    [[nodiscard]] uint64_t active() const
    {
        return user + nice + system + irq + softirq + steal + guest + guestNice;
    }
};

/// Raw memory counters from OS (in bytes or pages, converted to bytes).
struct MemoryCounters
{
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t availableBytes = 0; // Available for starting new apps (includes cached)
    uint64_t buffersBytes = 0;
    uint64_t cachedBytes = 0;

    uint64_t swapTotalBytes = 0;
    uint64_t swapFreeBytes = 0;
};

/// Combined system counters snapshot.
struct SystemCounters
{
    CpuCounters cpuTotal;                // Aggregate across all cores
    std::vector<CpuCounters> cpuPerCore; // Per-core (optional)
    MemoryCounters memory;

    uint64_t uptimeSeconds = 0;
    uint64_t bootTimestamp = 0; // Unix epoch

    // Load average (1, 5, 15 minute)
    double loadAvg1 = 0.0;
    double loadAvg5 = 0.0;
    double loadAvg15 = 0.0;

    // CPU frequency in MHz (current, may vary per-core)
    uint64_t cpuFreqMHz = 0;

    // Static system info (populated once)
    std::string hostname;
    std::string cpuModel;
    int cpuCoreCount = 0;
};

/// Reports what this platform's system probe supports.
struct SystemCapabilities
{
    bool hasPerCoreCpu = false;
    bool hasMemoryAvailable = false; // Some older kernels lack MemAvailable
    bool hasSwap = false;
    bool hasUptime = false;
    bool hasIoWait = false;
    bool hasSteal = false;
    bool hasLoadAvg = false;
    bool hasCpuFreq = false;
};

} // namespace Platform
