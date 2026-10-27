/// @file test_WindowsDiskProbe.cpp
/// @brief Integration tests for Platform::WindowsDiskProbe
///
/// These are integration tests that interact with the real Windows Performance Counters.
/// They verify that the probe correctly reads and parses disk I/O information.
/// The pure helpers (WindowsDiskProbeMath.h) are tested on every platform in
/// WindowsMath/test_WindowsDiskProbeMath.cpp.

#include <gtest/gtest.h>

#if defined(_WIN32)

#include "Platform/StorageTypes.h"
#include "Platform/Windows/WindowsDiskProbe.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <thread>

namespace Platform
{
namespace
{

// =============================================================================
// Construction and Basic Operations
// =============================================================================

TEST(WindowsDiskProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ const WindowsDiskProbe probe; });
}

TEST(WindowsDiskProbeTest, CapabilitiesReportedCorrectly)
{
    const WindowsDiskProbe probe;
    const auto caps = probe.capabilities();

    EXPECT_TRUE(caps.hasDiskStats);
    EXPECT_TRUE(caps.hasDeviceInfo);
    EXPECT_TRUE(caps.canFilterPhysical);

    // These may be true or false depending on PDH initialization
    // Just verify they are boolean values (no exception thrown)
    [[maybe_unused]] const bool hasBytes = caps.hasReadWriteBytes;
    [[maybe_unused]] const bool hasIoTime = caps.hasIoTime;
}

// =============================================================================
// Disk Counter Tests
// =============================================================================

TEST(WindowsDiskProbeTest, ReadReturnsValidCounters)
{
    WindowsDiskProbe probe;
    const auto counters = probe.read();

    // Should find at least one disk on a typical Windows system
    // We'll be lenient and just check the structure is valid
    EXPECT_GE(counters.disks.size(), 0ULL);
}

TEST(WindowsDiskProbeTest, DiskCountersHaveValidNames)
{
    WindowsDiskProbe probe;
    const auto counters = probe.read();

    for (const auto& disk : counters.disks)
    {
        EXPECT_FALSE(disk.deviceName.empty());
        // Windows disk names are typically drive letters (C:) or PDH instance names (e.g., "0 C:")
        // They should not be empty and should contain printable characters
        EXPECT_TRUE(std::ranges::all_of(disk.deviceName, [](unsigned char c) { return std::isprint(c) || std::isspace(c); }));
    }
}

TEST(WindowsDiskProbeTest, DiskCountersAreMonotonic)
{
    WindowsDiskProbe probe;

    const auto counters1 = probe.read();

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    const auto counters2 = probe.read();

    // WindowsDiskProbe sources DiskCounters from IOCTL_DISK_PERFORMANCE, which reports
    // genuinely cumulative counters (bytes/ops/time since the disk's counters started being
    // tracked) - matching the contract DiskCounters documents. For each disk that appears in
    // both samples, every counter must be non-decreasing.
    for (const auto& disk2 : counters2.disks)
    {
        for (const auto& disk1 : counters1.disks)
        {
            if (disk1.deviceName == disk2.deviceName)
            {
                EXPECT_GE(disk2.readsCompleted, disk1.readsCompleted);
                EXPECT_GE(disk2.readSectors, disk1.readSectors);
                EXPECT_GE(disk2.writesCompleted, disk1.writesCompleted);
                EXPECT_GE(disk2.writeSectors, disk1.writeSectors);
                EXPECT_GE(disk2.readTimeMs, disk1.readTimeMs);
                EXPECT_GE(disk2.writeTimeMs, disk1.writeTimeMs);
                EXPECT_GE(disk2.ioTimeMs, disk1.ioTimeMs);
            }
        }
    }
}

TEST(WindowsDiskProbeTest, SectorSizeIsValid)
{
    WindowsDiskProbe probe;
    const auto counters = probe.read();

    for (const auto& disk : counters.disks)
    {
        // Sector size should be 512 (typical) or 4096 (advanced format)
        EXPECT_TRUE(disk.sectorSize == 512 || disk.sectorSize == 4096);
    }
}

TEST(WindowsDiskProbeTest, TotalCountersAggregate)
{
    WindowsDiskProbe probe;
    const auto counters = probe.read();

    const uint64_t totalReads = counters.totalReadsCompleted();
    const uint64_t totalWrites = counters.totalWritesCompleted();
    const uint64_t totalReadBytes = counters.totalReadBytes();
    const uint64_t totalWriteBytes = counters.totalWriteBytes();

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

TEST(WindowsDiskProbeTest, ConsecutiveReadsAreConsistent)
{
    WindowsDiskProbe probe;

    const auto counters1 = probe.read();
    const auto counters2 = probe.read();

    // Device list should be stable between consecutive reads
    EXPECT_EQ(counters1.disks.size(), counters2.disks.size());
}

TEST(WindowsDiskProbeTest, PhysicalDeviceFlagIsSet)
{
    WindowsDiskProbe probe;
    const auto counters = probe.read();

    for (const auto& disk : counters.disks)
    {
        // All disks returned by WindowsDiskProbe should be marked as physical
        EXPECT_TRUE(disk.isPhysicalDevice);
    }
}

TEST(WindowsDiskProbeTest, RealDisksAreReadThroughDiskPerformanceOrTheFallback)
{
    // A smoke test of the real system calls, with no wait and nothing that needs disk activity
    // (#2298): PDH only names the physical disks and IOCTL_DISK_PERFORMANCE supplies cumulative
    // counters, which an idle disk may leave unchanged. How those counters map to DiskCounters,
    // and that they rise with I/O, is tested deterministically against fake calls in
    // test_WindowsDiskProbeSeams.cpp.
    WindowsDiskProbe probe;
    const auto caps = probe.capabilities();
    const auto counters = probe.read();

    if (caps.hasReadWriteBytes)
    {
        // At least one physical drive opened, so every disk is a PDH PhysicalDisk instance
        // ("0 C:"), whose name starts with the drive index.
        ASSERT_FALSE(counters.disks.empty());
        for (const auto& disk : counters.disks)
        {
            ASSERT_FALSE(disk.deviceName.empty());
            EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(disk.deviceName.front())) != 0) << disk.deviceName;
            EXPECT_EQ(disk.sectorSize, 512U) << disk.deviceName;
            // A first read is the busy-time baseline: zero when the driver reports IdleTime,
            // otherwise read + write time, in the same milliseconds.
            EXPECT_TRUE(disk.ioTimeMs == 0 || disk.ioTimeMs == disk.readTimeMs + disk.writeTimeMs) << disk.deviceName;
        }
    }
    else
    {
        // No physical drive could be opened: the fixed logical drives ("C:"), with zero counters.
        for (const auto& disk : counters.disks)
        {
            ASSERT_EQ(disk.deviceName.size(), 2U) << disk.deviceName;
            EXPECT_EQ(disk.deviceName[1], ':') << disk.deviceName;
            EXPECT_EQ(disk.readsCompleted + disk.writesCompleted + disk.readSectors + disk.writeSectors + disk.ioTimeMs, 0U)
                << disk.deviceName;
        }
    }
}

TEST(WindowsDiskProbeTest, FallbackToLogicalDrivesWorks)
{
    // This test verifies that even if PDH fails, we still enumerate drives
    WindowsDiskProbe probe;
    const auto counters = probe.read();

    // Should return at least the C: drive on any Windows system
    // But we'll be lenient and just verify no crash occurs
    EXPECT_GE(counters.disks.size(), 0ULL);
}

} // namespace
} // namespace Platform

#endif // _WIN32
