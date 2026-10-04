#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Domain
{

/// CPU usage percentages (computed from counter deltas).
struct CpuUsage
{
    double totalPercent = 0.0;  // Overall CPU busy % (iowait counts as idle, not busy; #1157)
    double userPercent = 0.0;   // User mode %
    double systemPercent = 0.0; // Kernel mode %
    double idlePercent = 0.0;   // Idle %
    double iowaitPercent = 0.0; // Waiting for I/O %
    double stealPercent = 0.0;  // Stolen by hypervisor %
};

/// Battery/power state snapshot for UI.
struct PowerStatus
{
    bool hasBattery = false;
    bool isOnAc = false;
    bool isCharging = false;
    bool isDischarging = false;
    bool isFull = false;
    bool isNotCharging = false; // Plugged in but held below full (#1158)

    // Charge percentage (0-100, or -1 if unavailable)
    int chargePercent = -1;

    // Power consumption in watts (positive = consuming, negative = charging)
    double powerWatts = 0.0;

    // Battery health (0-100, or -1 if unavailable)
    int healthPercent = -1;

    // Time remaining in seconds (0 if unavailable)
    std::uint64_t timeToEmptySec = 0;
    std::uint64_t timeToFullSec = 0;

    // Battery details
    std::string technology;
    std::string model;
};

/// Immutable, UI-ready system metrics snapshot.
/// Computed from raw counter deltas by SystemModel.
struct SystemSnapshot
{
    // CPU usage
    CpuUsage cpuTotal;
    // Per-core usage, indexed by core id (Platform::CpuCounters::coreId, the Linux cpuN), not by
    // the probe's list position. A core id with no reading this sample -- offline now, or just
    // come online with no previous sample to diff against -- holds NaN in every field, a gap
    // rather than a fake 0% or another core's load (#1229).
    std::vector<CpuUsage> cpuPerCore;

    // Memory (bytes)
    std::uint64_t memoryTotalBytes = 0;
    std::uint64_t memoryUsedBytes = 0;
    std::uint64_t memoryAvailableBytes = 0;
    std::uint64_t memoryCachedBytes = 0;
    std::uint64_t memoryBuffersBytes = 0;

    // Swap (bytes)
    std::uint64_t swapTotalBytes = 0;
    std::uint64_t swapUsedBytes = 0;

    // Computed percentages
    double memoryUsedPercent = 0.0;
    double memoryCachedPercent = 0.0;
    double swapUsedPercent = 0.0;

    // System info
    std::uint64_t uptimeSeconds = 0;
    int coreCount = 0;
    std::string hostname;
    std::string cpuModel;

    // Load average (1, 5, 15 minute) - Linux only
    double loadAvg1 = 0.0;
    double loadAvg5 = 0.0;
    double loadAvg15 = 0.0;

    // CPU frequency in MHz
    std::uint64_t cpuFreqMHz = 0;

    // Network rates (bytes per second, computed from counter deltas)
    double netRxBytesPerSec = 0.0;
    double netTxBytesPerSec = 0.0;

    /// Per-interface network rates (computed from counter deltas).
    struct InterfaceSnapshot
    {
        std::string name;           // System name: "eth0", "Ethernet"
        std::string displayName;    // Friendly name for UI
        double rxBytesPerSec = 0.0; // Receive rate
        double txBytesPerSec = 0.0; // Transmit rate
        bool isUp = false;          // Interface operational status
        uint64_t linkSpeedMbps = 0; // Link speed (0 if unknown)
    };
    std::vector<InterfaceSnapshot> networkInterfaces;

    // Power/battery status
    PowerStatus power;
};

} // namespace Domain
