#pragma once

#include <cstdint>
#include <vector>

namespace Domain
{

/// CPU usage percentages (computed from counter deltas).
struct CpuUsage
{
    double totalPercent = 0.0;  // Overall CPU busy %
    double userPercent = 0.0;   // User mode %
    double systemPercent = 0.0; // Kernel mode %
    double idlePercent = 0.0;   // Idle %
    double iowaitPercent = 0.0; // Waiting for I/O %
    double stealPercent = 0.0;  // Stolen by hypervisor %
};

/// Immutable, UI-ready system metrics snapshot.
/// Computed from raw counter deltas by SystemModel.
struct SystemSnapshot
{
    // CPU usage
    CpuUsage cpuTotal;
    std::vector<CpuUsage> cpuPerCore;

    // Memory (bytes)
    uint64_t memoryTotalBytes = 0;
    uint64_t memoryUsedBytes = 0;
    uint64_t memoryAvailableBytes = 0;
    uint64_t memoryCachedBytes = 0;
    uint64_t memoryBuffersBytes = 0;

    // Swap (bytes)
    uint64_t swapTotalBytes = 0;
    uint64_t swapUsedBytes = 0;

    // Computed percentages
    double memoryUsedPercent = 0.0;
    double swapUsedPercent = 0.0;

    // System info
    uint64_t uptimeSeconds = 0;
    int coreCount = 0;
};

} // namespace Domain
