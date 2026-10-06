#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Domain
{

/// Computed disk I/O metrics for a single device (derived from counter deltas).
/// Immutable snapshot for UI consumption.
struct DiskSnapshot
{
    std::string deviceName;

    // Rates (per second)
    double readBytesPerSec = 0.0;
    double writeBytesPerSec = 0.0;
    double readOpsPerSec = 0.0;
    double writeOpsPerSec = 0.0;

    // Utilization (0-100%)
    double utilizationPercent = 0.0;

    // Cumulative totals (useful for displaying absolute values)
    uint64_t totalReadBytes = 0;
    uint64_t totalWriteBytes = 0;
    uint64_t totalReadOps = 0;
    uint64_t totalWriteOps = 0;

    // Average I/O times (milliseconds)
    double avgReadTimeMs = 0.0;
    double avgWriteTimeMs = 0.0;

    bool isPhysicalDevice = true;
    // Whether the rate fields were measured. False for a disk's first sample and for a seed
    // transition too short to measure, whose rates read 0; their history records a gap (#1102).
    bool hasRates = false;
    // The sample's rates were thrown out as a counter glitch (over MAX_SANE_DISK_RATE_BPS, #1291):
    // unmeasured like a first sample, but the system Total must also be a gap rather than the sum of
    // the other disks, which would plot a false dip (#1327 review). A disk merely new to the sample
    // doesn't gap the Total.
    bool ratesRejected = false;
};

/// Aggregate storage metrics across all devices.
struct StorageSnapshot
{
    std::vector<DiskSnapshot> disks;

    // System-wide totals
    double totalReadBytesPerSec = 0.0;
    double totalWriteBytesPerSec = 0.0;
    double totalReadOpsPerSec = 0.0;
    double totalWriteOpsPerSec = 0.0;

    // Capabilities (what data is available)
    bool hasDiskStats = false;
    bool hasReadWriteBytes = false;
    bool hasIoTime = false;
};

} // namespace Domain
