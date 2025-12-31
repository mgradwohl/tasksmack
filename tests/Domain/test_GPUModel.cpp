/// @file test_GPUModel.cpp
/// @brief Comprehensive tests for Domain::GPUModel
///
/// Tests cover:
/// - GPU enumeration and snapshot creation
/// - Memory utilization percentage calculations
/// - Power utilization percentage calculations
/// - PCIe bandwidth rate calculations from counter deltas
/// - Multi-GPU scenarios
/// - Capability reporting
/// - Thread-safe operations

#include "Domain/GPUModel.h"
#include "Mocks/MockGPUProbe.h"
#include "Platform/GPUTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

using TestMocks::makeGPUCounters;
using TestMocks::makeGPUInfo;
using TestMocks::MockGPUProbe;

namespace
{

// =============================================================================
// Construction Tests
// =============================================================================

TEST(GPUModelTest, ConstructWithValidProbe)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU 0", "TestVendor");

    Domain::GPUModel model(std::move(probe));

    auto gpuInfo = model.gpuInfo();
    ASSERT_EQ(gpuInfo.size(), 1);
    EXPECT_EQ(gpuInfo[0].id, "GPU0");
    EXPECT_EQ(gpuInfo[0].name, "Test GPU 0");
    EXPECT_EQ(gpuInfo[0].vendor, "TestVendor");
}

TEST(GPUModelTest, ConstructWithNullProbeDoesNotCrash)
{
    Domain::GPUModel model(nullptr);
    model.refresh(); // Should not crash

    EXPECT_TRUE(model.snapshots().empty());
    EXPECT_TRUE(model.gpuInfo().empty());
}

TEST(GPUModelTest, CapabilitiesAreExposedFromProbe)
{
    auto probe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasTemperature = true;
    caps.hasPowerMetrics = true;
    caps.hasPerProcessMetrics = true;
    probe->withCapabilities(caps);

    Domain::GPUModel model(std::move(probe));

    const auto& modelCaps = model.capabilities();
    EXPECT_TRUE(modelCaps.hasTemperature);
    EXPECT_TRUE(modelCaps.hasPowerMetrics);
    EXPECT_TRUE(modelCaps.hasPerProcessMetrics);
}

// =============================================================================
// Single GPU Refresh Tests
// =============================================================================

TEST(GPUModelTest, FirstRefreshPopulatesSnapshot)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor")
        .withUtilization("GPU0", 75.0)
        .withMemory("GPU0", 2ULL * 1024 * 1024 * 1024, 8ULL * 1024 * 1024 * 1024);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    const auto& snap = snaps[0];
    EXPECT_EQ(snap.gpuId, "GPU0");
    EXPECT_EQ(snap.name, "Test GPU");
    EXPECT_EQ(snap.vendor, "TestVendor");
    EXPECT_DOUBLE_EQ(snap.utilizationPercent, 75.0);
    EXPECT_EQ(snap.memoryUsedBytes, 2ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(snap.memoryTotalBytes, 8ULL * 1024 * 1024 * 1024);
}

TEST(GPUModelTest, MemoryUtilizationPercentIsComputed)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor").withMemory("GPU0", 3ULL * 1024 * 1024 * 1024, 12ULL * 1024 * 1024 * 1024);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // 3GB / 12GB = 25%
    EXPECT_DOUBLE_EQ(snaps[0].memoryUsedPercent, 25.0);
}

TEST(GPUModelTest, PowerUtilizationPercentIsComputed)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto counters = makeGPUCounters("GPU0");
    counters.powerDrawWatts = 150.0;
    counters.powerLimitWatts = 300.0;
    probe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // 150W / 300W = 50%
    EXPECT_DOUBLE_EQ(snaps[0].powerUtilPercent, 50.0);
}

// =============================================================================
// PCIe Bandwidth Rate Tests
// =============================================================================

TEST(GPUModelTest, FirstRefreshShowsZeroPCIeRates)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto counters = makeGPUCounters("GPU0");
    counters.pcieTxBytes = 1000;
    counters.pcieRxBytes = 2000;
    probe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // No previous data, rates should be zero
    EXPECT_DOUBLE_EQ(snaps[0].pcieTxBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snaps[0].pcieRxBytesPerSec, 0.0);
}

TEST(GPUModelTest, SubsequentRefreshComputesPCIeRates)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    auto counters1 = makeGPUCounters("GPU0");
    counters1.pcieTxBytes = 1000;
    counters1.pcieRxBytes = 2000;
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters1);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    // Sleep for a known duration
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Update counters with deltas
    auto counters2 = makeGPUCounters("GPU0");
    counters2.pcieTxBytes = 2000; // +1000 bytes
    counters2.pcieRxBytes = 4000; // +2000 bytes
    rawProbe->withGPUCounters("GPU0", counters2);

    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Rates should be positive (exact values depend on timing)
    EXPECT_GT(snaps[0].pcieTxBytesPerSec, 0.0);
    EXPECT_GT(snaps[0].pcieRxBytesPerSec, 0.0);

    // Rough sanity check: ~1000 bytes / ~0.1 sec = ~10,000 bytes/sec
    EXPECT_GT(snaps[0].pcieTxBytesPerSec, 8000.0); // Allow timing variance
    EXPECT_LT(snaps[0].pcieTxBytesPerSec, 12000.0);
}

TEST(GPUModelTest, PCIeCounterRollbackHandled)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    auto counters1 = makeGPUCounters("GPU0");
    counters1.pcieTxBytes = 1000;
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters1);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Counter went backward (e.g., GPU reset)
    auto counters2 = makeGPUCounters("GPU0");
    counters2.pcieTxBytes = 500;
    rawProbe->withGPUCounters("GPU0", counters2);

    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Rate should be zero when counter decreases
    EXPECT_DOUBLE_EQ(snaps[0].pcieTxBytesPerSec, 0.0);
}

// =============================================================================
// Multi-GPU Tests
// =============================================================================

TEST(GPUModelTest, MultipleGPUsTrackedIndependently)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "GPU Zero", "VendorA")
        .withUtilization("GPU0", 50.0)
        .withGPU("GPU1", "GPU One", "VendorB")
        .withUtilization("GPU1", 75.0);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);

    // Find each GPU in snapshots
    const Domain::GPUSnapshot* gpu0 = nullptr;
    const Domain::GPUSnapshot* gpu1 = nullptr;
    for (const auto& snap : snaps)
    {
        if (snap.gpuId == "GPU0")
            gpu0 = &snap;
        else if (snap.gpuId == "GPU1")
            gpu1 = &snap;
    }

    ASSERT_NE(gpu0, nullptr);
    ASSERT_NE(gpu1, nullptr);

    EXPECT_EQ(gpu0->name, "GPU Zero");
    EXPECT_EQ(gpu0->vendor, "VendorA");
    EXPECT_DOUBLE_EQ(gpu0->utilizationPercent, 50.0);

    EXPECT_EQ(gpu1->name, "GPU One");
    EXPECT_EQ(gpu1->vendor, "VendorB");
    EXPECT_DOUBLE_EQ(gpu1->utilizationPercent, 75.0);
}

TEST(GPUModelTest, HistoryMaintainedPerGPU)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "GPU Zero", "VendorA").withGPU("GPU1", "GPU One", "VendorB");

    Domain::GPUModel model(std::move(probe));

    // First refresh
    model.refresh();

    // Second refresh with different values
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rawProbe->withUtilization("GPU0", 60.0).withUtilization("GPU1", 80.0);
    model.refresh();

    // Check history for each GPU
    const auto& hist0 = model.history("GPU0");
    const auto& hist1 = model.history("GPU1");

    EXPECT_EQ(hist0.size(), 2);
    EXPECT_EQ(hist1.size(), 2);

    // Verify latest values using the latest() method
    EXPECT_DOUBLE_EQ(hist0.latest().utilizationPercent, 60.0);
    EXPECT_DOUBLE_EQ(hist1.latest().utilizationPercent, 80.0);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST(GPUModelTest, ConcurrentReadsDuringRefresh)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> readCount{0};

    // Reader thread
    auto reader = std::jthread(
        [&](std::stop_token st)
        {
            while (!stop.load() && !st.stop_requested())
            {
                auto snaps = model.snapshots();
                EXPECT_LE(snaps.size(), 1);
                ++readCount;
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });

    // Perform several refreshes
    for (int i = 0; i < 10; ++i)
    {
        model.refresh();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop.store(true);
    reader.join();

    // Verify readers executed
    EXPECT_GT(readCount.load(), 0);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(GPUModelTest, ZeroMemoryTotalDoesNotCrash)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor").withMemory("GPU0", 1000, 0);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Should not divide by zero
    EXPECT_DOUBLE_EQ(snaps[0].memoryUsedPercent, 0.0);
}

TEST(GPUModelTest, ZeroPowerLimitDoesNotCrash)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto counters = makeGPUCounters("GPU0");
    counters.powerDrawWatts = 100.0;
    counters.powerLimitWatts = 0.0;
    probe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Should not divide by zero
    EXPECT_DOUBLE_EQ(snaps[0].powerUtilPercent, 0.0);
}

TEST(GPUModelTest, HistoryForNonexistentGPUReturnsEmpty)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    const auto& hist = model.history("NonexistentGPU");
    EXPECT_EQ(hist.size(), 0);
}

} // namespace
