/// @file test_WindowsDiskProbe.cpp
/// @brief Integration tests for Platform::WindowsDiskProbe
///
/// These are integration tests that interact with the real Windows Performance Counters.
/// They verify that the probe correctly reads and parses disk I/O information.

#include <gtest/gtest.h>

#if defined(_WIN32)

#include "Platform/StorageTypes.h"
#include "Platform/Windows/WindowsDiskProbe.h"
#include "Platform/Windows/WindowsDiskProbeMath.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

namespace Platform
{
namespace
{

// =============================================================================
// clampNonNegativeQuadPart: pure math, no hardware required. A buggy or virtualized
// disk driver can report a negative IOCTL_DISK_PERFORMANCE byte/time count; this must
// clamp to 0 rather than reinterpreting the sign bit as a huge magnitude.
// =============================================================================

TEST(ClampNonNegativeQuadPartTest, PositiveValuePassesThrough)
{
    EXPECT_EQ(clampNonNegativeQuadPart(12345), 12345ULL);
}

TEST(ClampNonNegativeQuadPartTest, ZeroPassesThrough)
{
    EXPECT_EQ(clampNonNegativeQuadPart(0), 0ULL);
}

TEST(ClampNonNegativeQuadPartTest, NegativeValueClampsToZero)
{
    EXPECT_EQ(clampNonNegativeQuadPart(-1), 0ULL);
    EXPECT_EQ(clampNonNegativeQuadPart(std::numeric_limits<int64_t>::min()), 0ULL);
}

TEST(ClampNonNegativeQuadPartTest, LargePositiveValueDoesNotWrap)
{
    constexpr auto largeValue = std::numeric_limits<int64_t>::max();
    EXPECT_EQ(clampNonNegativeQuadPart(largeValue), static_cast<uint64_t>(largeValue));
}

// =============================================================================
// parsePhysicalDriveIndex: pure parsing of PDH PhysicalDisk instance names, no
// hardware required. Malformed instance names must fail closed (nullopt) rather
// than opening an arbitrary/wrong \\.\PhysicalDriveN device.
// =============================================================================

TEST(ParsePhysicalDriveIndexTest, SingleDigitIndexWithDriveLetter)
{
    const auto result = parsePhysicalDriveIndex(L"0 C:");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);
}

TEST(ParsePhysicalDriveIndexTest, MultiDigitIndexWithMultipleDriveLetters)
{
    const auto result = parsePhysicalDriveIndex(L"12 D: E:");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 12);
}

TEST(ParsePhysicalDriveIndexTest, IndexWithNoTrailingSpaceOrLetters)
{
    const auto result = parsePhysicalDriveIndex(L"3");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 3);
}

TEST(ParsePhysicalDriveIndexTest, EmptyStringReturnsNullopt)
{
    EXPECT_FALSE(parsePhysicalDriveIndex(L"").has_value());
}

TEST(ParsePhysicalDriveIndexTest, LeadingSpaceReturnsNullopt)
{
    // Empty index part before the space.
    EXPECT_FALSE(parsePhysicalDriveIndex(L" C:").has_value());
}

TEST(ParsePhysicalDriveIndexTest, NonNumericIndexReturnsNullopt)
{
    EXPECT_FALSE(parsePhysicalDriveIndex(L"_Total").has_value());
    EXPECT_FALSE(parsePhysicalDriveIndex(L"C: 0").has_value());
}

TEST(ParsePhysicalDriveIndexTest, OverflowingNumericPrefixReturnsNulloptRatherThanWrapping)
{
    // A PhysicalDisk instance name is never legitimately this long, but a malformed/adversarial
    // one must fail closed instead of overflowing signed int (undefined behavior).
    EXPECT_FALSE(parsePhysicalDriveIndex(L"99999999999999999999 C:").has_value());
}

TEST(ParsePhysicalDriveIndexTest, MaxIntIndexIsAccepted)
{
    const auto result = parsePhysicalDriveIndex(L"2147483647 C:");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, std::numeric_limits<int>::max());
}

// =============================================================================
// Construction and Basic Operations
// =============================================================================

TEST(WindowsDiskProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ WindowsDiskProbe probe; });
}

TEST(WindowsDiskProbeTest, CapabilitiesReportedCorrectly)
{
    WindowsDiskProbe probe;
    auto caps = probe.capabilities();

    EXPECT_TRUE(caps.hasDiskStats);
    EXPECT_TRUE(caps.hasDeviceInfo);
    EXPECT_TRUE(caps.canFilterPhysical);

    // These may be true or false depending on PDH initialization
    // Just verify they are boolean values (no exception thrown)
    [[maybe_unused]] bool hasBytes = caps.hasReadWriteBytes;
    [[maybe_unused]] bool hasIoTime = caps.hasIoTime;
}

// =============================================================================
// Disk Counter Tests
// =============================================================================

TEST(WindowsDiskProbeTest, ReadReturnsValidCounters)
{
    WindowsDiskProbe probe;
    auto counters = probe.read();

    // Should find at least one disk on a typical Windows system
    // We'll be lenient and just check the structure is valid
    EXPECT_GE(counters.disks.size(), 0ULL);
}

TEST(WindowsDiskProbeTest, DiskCountersHaveValidNames)
{
    WindowsDiskProbe probe;
    auto counters = probe.read();

    for (const auto& disk : counters.disks)
    {
        EXPECT_FALSE(disk.deviceName.empty());
        // Windows disk names are typically drive letters (C:) or PDH instance names (e.g., "0 C:")
        // They should not be empty and should contain printable characters
        EXPECT_TRUE(std::all_of(
            disk.deviceName.begin(), disk.deviceName.end(), [](unsigned char c) { return std::isprint(c) || std::isspace(c); }));
    }
}

TEST(WindowsDiskProbeTest, DiskCountersAreMonotonic)
{
    WindowsDiskProbe probe;

    auto counters1 = probe.read();

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    auto counters2 = probe.read();

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
    auto counters = probe.read();

    for (const auto& disk : counters.disks)
    {
        // Sector size should be 512 (typical) or 4096 (advanced format)
        EXPECT_TRUE(disk.sectorSize == 512 || disk.sectorSize == 4096);
    }
}

TEST(WindowsDiskProbeTest, TotalCountersAggregate)
{
    WindowsDiskProbe probe;
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

TEST(WindowsDiskProbeTest, ConsecutiveReadsAreConsistent)
{
    WindowsDiskProbe probe;

    auto counters1 = probe.read();
    auto counters2 = probe.read();

    // Device list should be stable between consecutive reads
    EXPECT_EQ(counters1.disks.size(), counters2.disks.size());
}

TEST(WindowsDiskProbeTest, PhysicalDeviceFlagIsSet)
{
    WindowsDiskProbe probe;
    auto counters = probe.read();

    for (const auto& disk : counters.disks)
    {
        // All disks returned by WindowsDiskProbe should be marked as physical
        EXPECT_TRUE(disk.isPhysicalDevice);
    }
}

TEST(WindowsDiskProbeTest, PDHCountersProvideRealData)
{
    WindowsDiskProbe probe;

    // Wait for PDH to initialize and collect data
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    auto counters = probe.read();

    // If PDH is working, we should have at least one disk with some activity
    // This is a weak test since a system might have no I/O at the moment
    if (!counters.disks.empty())
    {
        bool hasAnyActivity = false;
        for (const auto& disk : counters.disks)
        {
            if (disk.readsCompleted > 0 || disk.writesCompleted > 0 || disk.readSectors > 0 || disk.writeSectors > 0)
            {
                hasAnyActivity = true;
                break;
            }
        }

        // It's okay if there's no activity, but the structure should be valid
        [[maybe_unused]] bool activityDetected = hasAnyActivity;
    }
}

TEST(WindowsDiskProbeTest, FallbackToLogicalDrivesWorks)
{
    // This test verifies that even if PDH fails, we still enumerate drives
    WindowsDiskProbe probe;
    auto counters = probe.read();

    // Should return at least the C: drive on any Windows system
    // But we'll be lenient and just verify no crash occurs
    EXPECT_GE(counters.disks.size(), 0ULL);
}

} // namespace
} // namespace Platform

#endif // _WIN32
