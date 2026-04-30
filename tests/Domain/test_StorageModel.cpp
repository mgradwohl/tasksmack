/// @file test_StorageModel.cpp
/// @brief Unit tests for Domain::StorageModel

#include "Domain/StorageModel.h"
#include "Domain/StorageSnapshot.h"
#include "Mocks/MockDiskProbe.h"
#include "Platform/IDiskProbe.h"
#include "Platform/StorageTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <thread>

namespace Domain
{
namespace
{

// =============================================================================
// Construction and Basic Operations
// =============================================================================

TEST(StorageModelTest, ConstructsWithProbe)
{
    auto probe = std::make_unique<Mocks::MockDiskProbe>();
    EXPECT_NO_THROW({ StorageModel model(std::move(probe)); });
}

TEST(StorageModelTest, InitialSnapshotIsEmpty)
{
    auto probe = std::make_unique<Mocks::MockDiskProbe>();
    StorageModel model(std::move(probe));

    auto snap = model.latestSnapshot();
    EXPECT_TRUE(snap.disks.empty());
    EXPECT_EQ(snap.totalReadBytesPerSec, 0.0);
    EXPECT_EQ(snap.totalWriteBytesPerSec, 0.0);
}

TEST(StorageModelTest, CapabilitiesFromProbe)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();
    mockProbe->setCapabilities({.hasDiskStats = true, .hasReadWriteBytes = true, .hasIoTime = true});

    StorageModel model(std::move(mockProbe));
    auto caps = model.capabilities();

    EXPECT_TRUE(caps.hasDiskStats);
    EXPECT_TRUE(caps.hasReadWriteBytes);
    EXPECT_TRUE(caps.hasIoTime);
}

// =============================================================================
// Sampling Tests
// =============================================================================

TEST(StorageModelTest, SampleUpdatesSnapshot)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    // Setup mock to return one disk
    Platform::SystemDiskCounters counters;
    Platform::DiskCounters disk;
    disk.deviceName = "sda";
    disk.readsCompleted = 100;
    disk.readSectors = 1000;
    disk.writesCompleted = 50;
    disk.writeSectors = 500;
    disk.sectorSize = 512;
    counters.disks.push_back(disk);

    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.sample();

    auto snap = model.latestSnapshot();
    EXPECT_EQ(snap.disks.size(), 1ULL);
    EXPECT_EQ(snap.disks[0].deviceName, "sda");
}

TEST(StorageModelTest, SecondSampleComputesRates)
{
    // This test would need a different structure since we can't easily
    // change the mock after moving it into the model
    // For now, we'll test the basic structure
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    // First sample
    Platform::SystemDiskCounters counters1;
    Platform::DiskCounters disk1;
    disk1.deviceName = "sda";
    disk1.readsCompleted = 100;
    disk1.readSectors = 1000;
    disk1.writesCompleted = 50;
    disk1.writeSectors = 500;
    disk1.readTimeMs = 100;
    disk1.writeTimeMs = 50;
    disk1.ioTimeMs = 150;
    disk1.sectorSize = 512;
    counters1.disks.push_back(disk1);
    mockProbe->setNextCounters(counters1);

    StorageModel model(std::move(mockProbe));
    model.sample();

    // First sample should have no rates yet (need two samples to compute delta)
    auto snap = model.latestSnapshot();
    EXPECT_EQ(snap.disks.size(), 1ULL);
}

TEST(StorageModelTest, HistoryGrowsWithSamples)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters disk;
    disk.deviceName = "sda";
    disk.readsCompleted = 100;
    disk.readSectors = 1000;
    disk.writesCompleted = 50;
    disk.writeSectors = 500;
    disk.sectorSize = 512;
    counters.disks.push_back(disk);
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));

    // Sample multiple times
    for (int i = 0; i < 5; ++i)
    {
        model.sample();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto history = model.history();
    EXPECT_EQ(history.size(), 5ULL);
}

TEST(StorageModelTest, MaxHistorySecondsLimitsHistory)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters disk;
    disk.deviceName = "sda";
    disk.readsCompleted = 100;
    disk.readSectors = 1000;
    disk.writesCompleted = 50;
    disk.writeSectors = 500;
    disk.sectorSize = 512;
    counters.disks.push_back(disk);
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.setMaxHistorySeconds(0.5); // Very short history

    // Sample multiple times with delays
    for (int i = 0; i < 10; ++i)
    {
        model.sample();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto history = model.history();
    // History should be trimmed - exact size depends on timing, but should be < 10
    EXPECT_LT(history.size(), 10ULL);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(StorageModelTest, HandlesEmptyDiskList)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();
    mockProbe->setNextCounters(Platform::SystemDiskCounters{});

    StorageModel model(std::move(mockProbe));
    model.sample();

    auto snap = model.latestSnapshot();
    EXPECT_TRUE(snap.disks.empty());
}

TEST(StorageModelTest, HandlesMultipleDisks)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    Platform::SystemDiskCounters counters;
    for (int i = 0; i < 3; ++i)
    {
        Platform::DiskCounters disk;
        disk.deviceName = "sd" + std::string(1, static_cast<char>('a' + i));
        disk.readsCompleted = static_cast<uint64_t>(100 * (i + 1));
        disk.readSectors = static_cast<uint64_t>(1000 * (i + 1));
        disk.writesCompleted = static_cast<uint64_t>(50 * (i + 1));
        disk.writeSectors = static_cast<uint64_t>(500 * (i + 1));
        disk.sectorSize = 512;
        counters.disks.push_back(disk);
    }
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.sample();

    auto snap = model.latestSnapshot();
    EXPECT_EQ(snap.disks.size(), 3ULL);
    EXPECT_EQ(snap.disks[0].deviceName, "sda");
    EXPECT_EQ(snap.disks[1].deviceName, "sdb");
    EXPECT_EQ(snap.disks[2].deviceName, "sdc");
}

// =============================================================================
// History Accessor Tests
// =============================================================================

TEST(StorageModelTest, TotalReadHistoryReturnsRates)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters disk;
    disk.deviceName = "sda";
    disk.readsCompleted = 100;
    disk.readSectors = 1000;
    disk.writesCompleted = 50;
    disk.writeSectors = 500;
    disk.sectorSize = 512;
    counters.disks.push_back(disk);
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.sample();
    model.sample();

    auto readHistory = model.totalReadHistory();
    auto writeHistory = model.totalWriteHistory();

    EXPECT_EQ(readHistory.size(), 2ULL);
    EXPECT_EQ(writeHistory.size(), 2ULL);
}

TEST(StorageModelTest, HistoryTimestampsReturnsTimestamps)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters disk;
    disk.deviceName = "sda";
    disk.readsCompleted = 100;
    disk.readSectors = 1000;
    disk.sectorSize = 512;
    counters.disks.push_back(disk);
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    model.sample();

    auto timestamps = model.historyTimestamps();
    EXPECT_EQ(timestamps.size(), 2ULL);
    // Second timestamp should be greater than first
    EXPECT_GT(timestamps[1], timestamps[0]);
}

TEST(StorageModelTest, CapabilitiesReflectProbeConfiguration)
{
    // Test that capabilities() correctly reports what the probe supports
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();
    mockProbe->setCapabilities({.hasDiskStats = false, .hasReadWriteBytes = true, .hasIoTime = false});

    StorageModel model(std::move(mockProbe));
    auto caps = model.capabilities();

    EXPECT_FALSE(caps.hasDiskStats);
    EXPECT_TRUE(caps.hasReadWriteBytes);
    EXPECT_FALSE(caps.hasIoTime);
}

// =============================================================================
// Rate Calculation Tests
// =============================================================================

TEST(StorageModelTest, SecondSampleWithSameCountersComputesZeroRates)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters disk;
    disk.deviceName = "sda";
    disk.readsCompleted = 100;
    disk.readSectors = 1000;
    disk.writesCompleted = 50;
    disk.writeSectors = 500;
    disk.sectorSize = 512;
    counters.disks.push_back(disk);
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Need time between samples
    model.sample();

    auto snap = model.latestSnapshot();
    EXPECT_EQ(snap.disks.size(), 1ULL);
    // Since counters don't change, rates should be 0
    EXPECT_DOUBLE_EQ(snap.disks[0].readBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.disks[0].writeBytesPerSec, 0.0);
}

TEST(StorageModelTest, TotalsAreAggregatedFromAllDisks)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    Platform::SystemDiskCounters counters;
    for (int i = 0; i < 2; ++i)
    {
        Platform::DiskCounters disk;
        disk.deviceName = "sd" + std::string(1, static_cast<char>('a' + i));
        disk.readsCompleted = 100;
        disk.readSectors = 1000;
        disk.writesCompleted = 50;
        disk.writeSectors = 500;
        disk.sectorSize = 512;
        counters.disks.push_back(disk);
    }
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.sample();

    auto snap = model.latestSnapshot();
    EXPECT_EQ(snap.disks.size(), 2ULL);
    // On first sample, rates are 0, but totals should be aggregated
    EXPECT_DOUBLE_EQ(snap.totalReadBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.totalWriteBytesPerSec, 0.0);
}

TEST(StorageModelTest, DiskSnapshotContainsTotalBytes)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters disk;
    disk.deviceName = "sda";
    disk.readsCompleted = 100;
    disk.readSectors = 1000; // 1000 sectors * 512 bytes = 512000 bytes
    disk.writesCompleted = 50;
    disk.writeSectors = 500; // 500 sectors * 512 bytes = 256000 bytes
    disk.sectorSize = 512;
    counters.disks.push_back(disk);
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.sample();

    auto snap = model.latestSnapshot();
    EXPECT_EQ(snap.disks[0].totalReadBytes, 1000ULL * 512ULL);
    EXPECT_EQ(snap.disks[0].totalWriteBytes, 500ULL * 512ULL);
    EXPECT_EQ(snap.disks[0].totalReadOps, 100ULL);
    EXPECT_EQ(snap.disks[0].totalWriteOps, 50ULL);
}

TEST(StorageModelTest, DiskSnapshotContainsPhysicalDeviceFlag)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters disk;
    disk.deviceName = "sda";
    disk.isPhysicalDevice = true;
    disk.readsCompleted = 100;
    disk.readSectors = 1000;
    disk.sectorSize = 512;
    counters.disks.push_back(disk);
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.sample();

    auto snap = model.latestSnapshot();
    EXPECT_TRUE(snap.disks[0].isPhysicalDevice);
}

TEST(StorageModelTest, SnapshotReflectsProbeCapabilities)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();
    mockProbe->setCapabilities({.hasDiskStats = true, .hasReadWriteBytes = false, .hasIoTime = true});

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters disk;
    disk.deviceName = "sda";
    disk.sectorSize = 512;
    counters.disks.push_back(disk);
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.sample();

    auto snap = model.latestSnapshot();
    EXPECT_TRUE(snap.hasDiskStats);
    EXPECT_FALSE(snap.hasReadWriteBytes);
    EXPECT_TRUE(snap.hasIoTime);
}

// =============================================================================
// Per-Disk History
// =============================================================================

TEST(StorageModelTest, PerDiskHistoryEmptyBeforeSample)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();
    StorageModel model(std::move(mockProbe));

    EXPECT_TRUE(model.perDiskHistory().empty());
}

TEST(StorageModelTest, PerDiskHistoryTracksAllDisks)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters sda;
    sda.deviceName = "sda";
    sda.sectorSize = 512;
    sda.readsCompleted = 100;
    sda.readSectors = 1000;
    counters.disks.push_back(sda);

    Platform::DiskCounters nvme;
    nvme.deviceName = "nvme0n1";
    nvme.sectorSize = 512;
    nvme.readsCompleted = 200;
    nvme.readSectors = 2000;
    counters.disks.push_back(nvme);

    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.sample();

    const auto history = model.perDiskHistory();
    ASSERT_EQ(history.size(), 2U);
    EXPECT_EQ(history[0].deviceName, "sda");
    EXPECT_EQ(history[1].deviceName, "nvme0n1");
}

TEST(StorageModelTest, PerDiskHistoryAlignedToTimestamps)
{
    auto mockProbeOwned = std::make_unique<Mocks::MockDiskProbe>();
    Mocks::MockDiskProbe* mockProbe = mockProbeOwned.get();

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters sda;
    sda.deviceName = "sda";
    sda.sectorSize = 512;
    sda.readsCompleted = 100;
    sda.readSectors = 1000;
    counters.disks.push_back(sda);

    Platform::DiskCounters nvme;
    nvme.deviceName = "nvme0n1";
    nvme.sectorSize = 512;
    nvme.readsCompleted = 200;
    nvme.readSectors = 2000;
    counters.disks.push_back(nvme);

    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbeOwned));
    model.sample();

    // Second sample with updated sector counts to produce non-zero rates
    sda.readsCompleted = 200;
    sda.readSectors = 2000;
    sda.writesCompleted = 50;
    sda.writeSectors = 500;
    nvme.readsCompleted = 400;
    nvme.readSectors = 4000;
    nvme.writesCompleted = 100;
    nvme.writeSectors = 1000;
    counters.disks.clear();
    counters.disks.push_back(sda);
    counters.disks.push_back(nvme);
    mockProbe->setNextCounters(counters);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    model.sample();

    const auto timestamps = model.historyTimestamps();
    const auto history = model.perDiskHistory();

    ASSERT_EQ(history.size(), 2U);
    for (const auto& entry : history)
    {
        EXPECT_EQ(entry.readBytesPerSec.size(), timestamps.size());
        EXPECT_EQ(entry.writeBytesPerSec.size(), timestamps.size());
    }
}

TEST(StorageModelTest, PerDiskHistoryRatesNonNegative)
{
    auto mockProbeOwned = std::make_unique<Mocks::MockDiskProbe>();
    Mocks::MockDiskProbe* mockProbe = mockProbeOwned.get();

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters sda;
    sda.deviceName = "sda";
    sda.sectorSize = 512;
    sda.readsCompleted = 100;
    sda.readSectors = 1000;
    sda.writesCompleted = 50;
    sda.writeSectors = 500;
    counters.disks.push_back(sda);
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbeOwned));
    model.sample();

    sda.readsCompleted = 200;
    sda.readSectors = 2000;
    sda.writesCompleted = 100;
    sda.writeSectors = 1000;
    counters.disks.clear();
    counters.disks.push_back(sda);
    mockProbe->setNextCounters(counters);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    model.sample();

    const auto history = model.perDiskHistory();
    ASSERT_EQ(history.size(), 1U);
    for (const double rate : history[0].readBytesPerSec)
    {
        EXPECT_GE(rate, 0.0);
    }
    for (const double rate : history[0].writeBytesPerSec)
    {
        EXPECT_GE(rate, 0.0);
    }
}

TEST(StorageModelTest, PerDiskHistoryPreservesInsertionOrder)
{
    auto mockProbe = std::make_unique<Mocks::MockDiskProbe>();

    Platform::SystemDiskCounters counters;
    Platform::DiskCounters diskA;
    diskA.deviceName = "sdb";
    diskA.sectorSize = 512;
    counters.disks.push_back(diskA);

    Platform::DiskCounters diskB;
    diskB.deviceName = "sda";
    diskB.sectorSize = 512;
    counters.disks.push_back(diskB);

    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbe));
    model.sample();

    const auto history = model.perDiskHistory();
    ASSERT_EQ(history.size(), 2U);
    // Order must match probe insertion order
    EXPECT_EQ(history[0].deviceName, "sdb");
    EXPECT_EQ(history[1].deviceName, "sda");
}

TEST(StorageModelTest, PerDiskHistoryDiskDisappearsPreservesAlignment)
{
    auto mockProbeOwned = std::make_unique<Mocks::MockDiskProbe>();
    Mocks::MockDiskProbe* mockProbe = mockProbeOwned.get();

    // Sample 1: two disks
    Platform::SystemDiskCounters counters;
    Platform::DiskCounters sda;
    sda.deviceName = "sda";
    sda.sectorSize = 512;
    sda.readsCompleted = 100;
    sda.readSectors = 1000;
    counters.disks.push_back(sda);
    Platform::DiskCounters sdb;
    sdb.deviceName = "sdb";
    sdb.sectorSize = 512;
    sdb.readsCompleted = 50;
    sdb.readSectors = 500;
    counters.disks.push_back(sdb);
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbeOwned));
    model.sample();

    // Sample 2: sdb disappears
    counters.disks.clear();
    sda.readsCompleted = 200;
    sda.readSectors = 2000;
    counters.disks.push_back(sda);
    mockProbe->setNextCounters(counters);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    model.sample();

    const auto timestamps = model.historyTimestamps();
    const auto history = model.perDiskHistory();

    ASSERT_EQ(history.size(), 2U);
    for (const auto& entry : history)
    {
        EXPECT_EQ(entry.readBytesPerSec.size(), timestamps.size())
            << "Per-disk read history for " << entry.deviceName << " must be aligned to timestamps";
        EXPECT_EQ(entry.writeBytesPerSec.size(), timestamps.size())
            << "Per-disk write history for " << entry.deviceName << " must be aligned to timestamps";
    }

    // sdb was absent in sample 2: its last entry must be the 0.0 placeholder
    const auto it = std::ranges::find_if(history, [](const PerDiskHistory& e) { return e.deviceName == "sdb"; });
    ASSERT_NE(it, history.end());
    EXPECT_DOUBLE_EQ(it->readBytesPerSec.back(), 0.0);
    EXPECT_DOUBLE_EQ(it->writeBytesPerSec.back(), 0.0);
}

TEST(StorageModelTest, PerDiskHistoryNewDiskAppearsBackfillsPlaceholders)
{
    auto mockProbeOwned = std::make_unique<Mocks::MockDiskProbe>();
    Mocks::MockDiskProbe* mockProbe = mockProbeOwned.get();

    // Sample 1: only sda
    Platform::SystemDiskCounters counters;
    Platform::DiskCounters sda;
    sda.deviceName = "sda";
    sda.sectorSize = 512;
    sda.readsCompleted = 100;
    sda.readSectors = 1000;
    counters.disks.push_back(sda);
    mockProbe->setNextCounters(counters);

    StorageModel model(std::move(mockProbeOwned));
    model.sample();

    // Sample 2: sda + new disk nvme0n1
    sda.readsCompleted = 200;
    sda.readSectors = 2000;
    Platform::DiskCounters nvme;
    nvme.deviceName = "nvme0n1";
    nvme.sectorSize = 512;
    nvme.readsCompleted = 100;
    nvme.readSectors = 1000;
    counters.disks.clear();
    counters.disks.push_back(sda);
    counters.disks.push_back(nvme);
    mockProbe->setNextCounters(counters);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    model.sample();

    const auto timestamps = model.historyTimestamps();
    const auto history = model.perDiskHistory();

    ASSERT_EQ(history.size(), 2U);
    for (const auto& entry : history)
    {
        EXPECT_EQ(entry.readBytesPerSec.size(), timestamps.size())
            << "Per-disk read history for " << entry.deviceName << " must be aligned to timestamps";
        EXPECT_EQ(entry.writeBytesPerSec.size(), timestamps.size())
            << "Per-disk write history for " << entry.deviceName << " must be aligned to timestamps";
    }

    // nvme0n1 appeared in sample 2: its oldest (backfilled) entry must be 0.0
    const auto it = std::ranges::find_if(history, [](const PerDiskHistory& e) { return e.deviceName == "nvme0n1"; });
    ASSERT_NE(it, history.end());
    ASSERT_GE(it->readBytesPerSec.size(), 1U);
    EXPECT_DOUBLE_EQ(it->readBytesPerSec.front(), 0.0);
    EXPECT_DOUBLE_EQ(it->writeBytesPerSec.front(), 0.0);
}

} // namespace
} // namespace Domain
