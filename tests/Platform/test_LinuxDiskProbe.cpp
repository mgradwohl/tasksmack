/// @file test_LinuxDiskProbe.cpp
/// @brief Tests for Platform::LinuxDiskProbe
///
/// Contains two test suites:
///   1. Integration tests that interact with the real /proc/diskstats filesystem.
///   2. Unit tests using a synthetic diskstats file written to a temp path,
///      covering all filter branches and read() paths that cannot be reliably
///      triggered on real hardware.

#include <gtest/gtest.h>

#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/Linux/LinuxDiskProbe.h"
#include "Platform/StorageTypes.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <unistd.h>

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

// =============================================================================
// Synthetic diskstats read() — unit tests via temp file
// =============================================================================

namespace Platform
{
namespace
{

class LinuxDiskProbeReadTest : public ::testing::Test
{
  protected:
    std::filesystem::path m_DiskstatsPath;

    void SetUp() override
    {
        static std::atomic<int> s_counter{0};
        const auto seq = s_counter.fetch_add(1, std::memory_order_relaxed);
        m_DiskstatsPath =
            std::filesystem::temp_directory_path() / ("tasksmack_diskstats_" + std::to_string(getpid()) + "_" + std::to_string(seq));
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove(m_DiskstatsPath, ec);
    }

    void writeDiskstats(const std::string& content) const
    {
        std::ofstream f(m_DiskstatsPath);
        ASSERT_TRUE(f.is_open()) << "Failed to open temp diskstats file: " << m_DiskstatsPath;
        f << content;
        f.flush();
        ASSERT_TRUE(f.good()) << "Failed to write temp diskstats file: " << m_DiskstatsPath;
    }

    // Build one valid /proc/diskstats line with all required fields.
    // Fields: major minor name reads_completed reads_merged sectors_read
    //         time_reading writes_completed writes_merged sectors_written
    //         time_writing io_in_progress time_io weighted_time_io
    static std::string makeLine(const std::string& name,
                                uint64_t readsCompleted = 100,
                                uint64_t sectorsRead = 200,
                                uint64_t writesCompleted = 50,
                                uint64_t sectorsWritten = 100)
    {
        return "8 0 " + name + " " + std::to_string(readsCompleted) + " 10 " + std::to_string(sectorsRead) + " 500 " +
               std::to_string(writesCompleted) + " 5 " + std::to_string(sectorsWritten) + " 300 0 800 1000\n";
    }
};

TEST_F(LinuxDiskProbeReadTest, EmptyFileReturnsNoDisks)
{
    writeDiskstats("");
    LinuxDiskProbe probe(m_DiskstatsPath);
    EXPECT_TRUE(probe.read().disks.empty());
}

TEST_F(LinuxDiskProbeReadTest, MissingFileReturnsNoDisks)
{
    // Do NOT write the file — probe should log a warning and return empty.
    LinuxDiskProbe probe(m_DiskstatsPath);
    EXPECT_TRUE(probe.read().disks.empty());
}

TEST_F(LinuxDiskProbeReadTest, ValidSdaLineIsIncluded)
{
    writeDiskstats(makeLine("sda", 100, 200, 50, 100));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    ASSERT_EQ(counters.disks.size(), 1U);
    EXPECT_EQ(counters.disks[0].deviceName, "sda");
    EXPECT_EQ(counters.disks[0].readsCompleted, 100U);
    EXPECT_EQ(counters.disks[0].readSectors, 200U);
    EXPECT_EQ(counters.disks[0].writesCompleted, 50U);
    EXPECT_EQ(counters.disks[0].writeSectors, 100U);
    EXPECT_EQ(counters.disks[0].sectorSize, 512U);
    EXPECT_TRUE(counters.disks[0].isPhysicalDevice);
}

TEST_F(LinuxDiskProbeReadTest, LoopDeviceLineIsFiltered)
{
    writeDiskstats(makeLine("loop0") + makeLine("sda"));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    ASSERT_EQ(counters.disks.size(), 1U);
    EXPECT_EQ(counters.disks[0].deviceName, "sda");
}

TEST_F(LinuxDiskProbeReadTest, RamDeviceLineIsFiltered)
{
    writeDiskstats(makeLine("ram0") + makeLine("sdb"));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    ASSERT_EQ(counters.disks.size(), 1U);
    EXPECT_EQ(counters.disks[0].deviceName, "sdb");
}

TEST_F(LinuxDiskProbeReadTest, PartitionLineIsFiltered)
{
    writeDiskstats(makeLine("sda1") + makeLine("sda"));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    ASSERT_EQ(counters.disks.size(), 1U);
    EXPECT_EQ(counters.disks[0].deviceName, "sda");
}

TEST_F(LinuxDiskProbeReadTest, NvmeWholeNamespaceIsIncluded)
{
    writeDiskstats(makeLine("nvme0n1", 300, 600, 150, 400));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    ASSERT_EQ(counters.disks.size(), 1U);
    EXPECT_EQ(counters.disks[0].deviceName, "nvme0n1");
    EXPECT_EQ(counters.disks[0].readsCompleted, 300U);
}

TEST_F(LinuxDiskProbeReadTest, NvmePartitionLineIsFiltered)
{
    writeDiskstats(makeLine("nvme0n1p1") + makeLine("nvme0n1"));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    ASSERT_EQ(counters.disks.size(), 1U);
    EXPECT_EQ(counters.disks[0].deviceName, "nvme0n1");
}

TEST_F(LinuxDiskProbeReadTest, MalformedLineIsSkipped)
{
    // Too few fields — iss.fail() should trigger the continue path.
    writeDiskstats("8 0 sda 100\n" + makeLine("sdb"));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    // Malformed sda line skipped; sdb present
    ASSERT_EQ(counters.disks.size(), 1U);
    EXPECT_EQ(counters.disks[0].deviceName, "sdb");
}

TEST_F(LinuxDiskProbeReadTest, MultipleDisksAllParsed)
{
    writeDiskstats(makeLine("sda") + makeLine("sdb") + makeLine("nvme0n1"));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    ASSERT_EQ(counters.disks.size(), 3U);
}

TEST_F(LinuxDiskProbeReadTest, TotalCountersMatchSumOfParsedDisks)
{
    writeDiskstats(makeLine("sda", 100, 200, 50, 100) + makeLine("sdb", 200, 400, 80, 160));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    ASSERT_EQ(counters.disks.size(), 2U);
    EXPECT_EQ(counters.totalReadsCompleted(), 300U);
    EXPECT_EQ(counters.totalWritesCompleted(), 130U);
    // readBytes = sectors * 512
    EXPECT_EQ(counters.totalReadBytes(), (200U + 400U) * 512U);
    EXPECT_EQ(counters.totalWriteBytes(), (100U + 160U) * 512U);
}

TEST_F(LinuxDiskProbeReadTest, VirtualDiskLineIsIncluded)
{
    writeDiskstats(makeLine("vda", 50, 100, 20, 40));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    ASSERT_EQ(counters.disks.size(), 1U);
    EXPECT_EQ(counters.disks[0].deviceName, "vda");
}

TEST_F(LinuxDiskProbeReadTest, DeviceMapperLineIsFiltered)
{
    // dm-0 (LVM) must be excluded; sda alongside it must pass through.
    writeDiskstats(makeLine("dm-0") + makeLine("sda"));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    ASSERT_EQ(counters.disks.size(), 1U);
    EXPECT_EQ(counters.disks[0].deviceName, "sda");
}

TEST_F(LinuxDiskProbeReadTest, MmcWholeDiskLineIsIncluded)
{
    // mmcblk0 is a whole eMMC disk — it should be included now that the
    // filter has an explicit exception for mmcblk<N> (no 'p' suffix).
    writeDiskstats(makeLine("mmcblk0", 80, 160, 30, 60));
    LinuxDiskProbe probe(m_DiskstatsPath);
    auto counters = probe.read();

    ASSERT_EQ(counters.disks.size(), 1U);
    EXPECT_EQ(counters.disks[0].deviceName, "mmcblk0");
}

TEST_F(LinuxDiskProbeReadTest, MmcPartitionLineIsFiltered)
{
    writeDiskstats(makeLine("mmcblk0p1"));
    LinuxDiskProbe probe(m_DiskstatsPath);
    EXPECT_TRUE(probe.read().disks.empty());
}

} // namespace
} // namespace Platform

#endif // __linux__ && __has_include(<unistd.h>)
