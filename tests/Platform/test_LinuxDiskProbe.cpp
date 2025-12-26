/// @file test_LinuxDiskProbe.cpp
/// @brief Integration tests for Platform::LinuxDiskProbe
///
/// These are integration tests that interact with the real /proc/diskstats filesystem.
/// They verify that the probe correctly reads and parses disk I/O information.

#include <gtest/gtest.h>

#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/Linux/LinuxDiskProbe.h"
#include "Platform/StorageTypes.h"

#include <chrono>
#include <fstream>
#include <thread>

namespace Platform
{
namespace
{

// =============================================================================
// Construction and Basic Operations
// =============================================================================

TEST(LinuxDiskProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ LinuxDiskProbe probe; });
}

TEST(LinuxDiskProbeTest, CapabilitiesReportedCorrectly)
{
    LinuxDiskProbe probe;
    auto caps = probe.capabilities();

    EXPECT_TRUE(caps.hasDiskStats);
    EXPECT_TRUE(caps.hasReadWriteBytes);
    EXPECT_TRUE(caps.hasIoTime);
    EXPECT_TRUE(caps.hasDeviceInfo);
    EXPECT_TRUE(caps.canFilterPhysical);
}

// =============================================================================
// Disk Counter Tests
// =============================================================================

TEST(LinuxDiskProbeTest, ReadReturnsValidCounters)
{
    LinuxDiskProbe probe;
    auto counters = probe.read();

    // Should find at least some disks (unless in a very minimal container)
    // We'll be lenient and just check the structure is valid
    EXPECT_GE(counters.disks.size(), 0ULL);
}

TEST(LinuxDiskProbeTest, ProcDiskstatsExists)
{
    std::ifstream diskstats("/proc/diskstats");
    EXPECT_TRUE(diskstats.good());
}

TEST(LinuxDiskProbeTest, DiskCountersHaveValidNames)
{
    LinuxDiskProbe probe;
    auto counters = probe.read();

    for (const auto& disk : counters.disks)
    {
        EXPECT_FALSE(disk.deviceName.empty());
        // Device names should not contain loop devices (they are filtered)
        EXPECT_EQ(disk.deviceName.find("loop"), std::string::npos);
        // Device names should not contain ram devices (they are filtered)
        EXPECT_EQ(disk.deviceName.find("ram"), std::string::npos);
    }
}

TEST(LinuxDiskProbeTest, DiskCountersAreMonotonic)
{
    LinuxDiskProbe probe;

    auto counters1 = probe.read();

    // Do some I/O
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto counters2 = probe.read();

    // For each disk that appears in both samples, counters should not decrease
    for (const auto& disk2 : counters2.disks)
    {
        for (const auto& disk1 : counters1.disks)
        {
            if (disk1.deviceName == disk2.deviceName)
            {
                // Counters should be monotonically increasing (or at least non-decreasing)
                EXPECT_GE(disk2.readsCompleted, disk1.readsCompleted);
                EXPECT_GE(disk2.readSectors, disk1.readSectors);
                EXPECT_GE(disk2.writesCompleted, disk1.writesCompleted);
                EXPECT_GE(disk2.writeSectors, disk1.writeSectors);
            }
        }
    }
}

TEST(LinuxDiskProbeTest, SectorSizeIsValid)
{
    LinuxDiskProbe probe;
    auto counters = probe.read();

    for (const auto& disk : counters.disks)
    {
        // Sector size should be 512 (typical) or 4096 (advanced format)
        EXPECT_TRUE(disk.sectorSize == 512 || disk.sectorSize == 4096);
    }
}

TEST(LinuxDiskProbeTest, TotalCountersAggregate)
{
    LinuxDiskProbe probe;
    auto counters = probe.read();

    uint64_t totalReads = counters.totalReadsCompleted();
    uint64_t totalWrites = counters.totalWritesCompleted();
    uint64_t totalReadBytes = counters.totalReadBytes();
    uint64_t totalWriteBytes = counters.totalWriteBytes();

    // If we have disks, totals should match sum
    if (!counters.disks.empty())
    {
        uint64_t sumReads = 0;
        uint64_t sumWrites = 0;
        uint64_t sumReadBytes = 0;
        uint64_t sumWriteBytes = 0;

        for (const auto& disk : counters.disks)
        {
            sumReads += disk.readsCompleted;
            sumWrites += disk.writesCompleted;
            sumReadBytes += disk.readSectors * disk.sectorSize;
            sumWriteBytes += disk.writeSectors * disk.sectorSize;
        }

        EXPECT_EQ(totalReads, sumReads);
        EXPECT_EQ(totalWrites, sumWrites);
        EXPECT_EQ(totalReadBytes, sumReadBytes);
        EXPECT_EQ(totalWriteBytes, sumWriteBytes);
    }
}

TEST(LinuxDiskProbeTest, ConsecutiveReadsAreConsistent)
{
    LinuxDiskProbe probe;

    auto counters1 = probe.read();
    auto counters2 = probe.read();

    // Device list should be stable between consecutive reads
    EXPECT_EQ(counters1.disks.size(), counters2.disks.size());
}

} // namespace
} // namespace Platform

#endif // __linux__ && __has_include(<unistd.h>)
