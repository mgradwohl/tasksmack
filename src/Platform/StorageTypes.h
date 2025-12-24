#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

/// Raw disk I/O counters from OS (cumulative counts).
/// Probes populate this; domain computes deltas and rates.
struct DiskCounters
{
    std::string deviceName;        // e.g., "sda", "nvme0n1", "C:"
    uint64_t readsCompleted = 0;   // Number of read operations completed
    uint64_t readSectors = 0;      // Number of sectors read
    uint64_t readTimeMs = 0;       // Time spent reading (milliseconds)
    uint64_t writesCompleted = 0;  // Number of write operations completed
    uint64_t writeSectors = 0;     // Number of sectors written
    uint64_t writeTimeMs = 0;      // Time spent writing (milliseconds)
    uint64_t ioInProgressMs = 0;   // I/O operations currently in progress
    uint64_t ioTimeMs = 0;         // Total time this device has been active (milliseconds)
    uint64_t weightedIoTimeMs = 0; // Weighted time of I/O operations

    // Device info (may not be available on all platforms)
    uint64_t sectorSize = 512;    // Sector size in bytes (typically 512 or 4096)
    bool isPhysicalDevice = true; // False for loop devices, partitions on some systems
};

/// Aggregate counters for all disks combined.
struct SystemDiskCounters
{
    std::vector<DiskCounters> disks;

    /// Total reads across all disks
    [[nodiscard]] uint64_t totalReadsCompleted() const
    {
        uint64_t total = 0;
        for (const auto& disk : disks)
        {
            total += disk.readsCompleted;
        }
        return total;
    }

    /// Total writes across all disks
    [[nodiscard]] uint64_t totalWritesCompleted() const
    {
        uint64_t total = 0;
        for (const auto& disk : disks)
        {
            total += disk.writesCompleted;
        }
        return total;
    }

    /// Total read bytes across all disks
    [[nodiscard]] uint64_t totalReadBytes() const
    {
        uint64_t total = 0;
        for (const auto& disk : disks)
        {
            total += disk.readSectors * disk.sectorSize;
        }
        return total;
    }

    /// Total write bytes across all disks
    [[nodiscard]] uint64_t totalWriteBytes() const
    {
        uint64_t total = 0;
        for (const auto& disk : disks)
        {
            total += disk.writeSectors * disk.sectorSize;
        }
        return total;
    }
};

/// Reports what this platform's disk probe supports.
struct DiskCapabilities
{
    bool hasDiskStats = false;      // Can read disk I/O statistics
    bool hasReadWriteBytes = false; // Can report bytes read/written
    bool hasIoTime = false;         // Can report time spent in I/O
    bool hasDeviceInfo = false;     // Can report device metadata (size, type)
    bool canFilterPhysical = false; // Can distinguish physical vs virtual devices
};

} // namespace Platform
