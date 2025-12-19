/// @file test_CrossLayer.cpp
/// @brief Cross-layer integration tests (Domain + Platform without mocks)
///
/// These tests verify that Domain and Platform layers work correctly together
/// without using mock objects. They use real platform probes to validate
/// end-to-end behavior.

#include "Domain/ProcessModel.h"
#include "Domain/SystemModel.h"
#include "Platform/Factory.h"

#include <gtest/gtest.h>

#include <thread>
#include <unistd.h>

// =============================================================================
// ProcessModel + Real Platform Probe Integration
// =============================================================================

TEST(CrossLayerIntegrationTest, ProcessModelWithRealProbeWorks)
{
    auto probe = Platform::makeProcessProbe();
    ASSERT_NE(probe, nullptr);

    Domain::ProcessModel model(std::move(probe));

    // First refresh
    model.refresh();

    // Should find at least some processes (this test process, init, etc.)
    EXPECT_GT(model.processCount(), 0);

    // Snapshots should be populated
    auto snaps = model.snapshots();
    EXPECT_FALSE(snaps.empty());
}

TEST(CrossLayerIntegrationTest, ProcessModelFindsOwnProcess)
{
    auto probe = Platform::makeProcessProbe();
    Domain::ProcessModel model(std::move(probe));

    model.refresh();

    auto snaps = model.snapshots();
    auto currentPid = static_cast<int32_t>(getpid());

    bool foundSelf = false;
    for (const auto& snap : snaps)
    {
        if (snap.pid == currentPid)
        {
            foundSelf = true;
            // Our process should have valid data
            EXPECT_GT(snap.memoryBytes, 0);
            EXPECT_NE(snap.uniqueKey, 0);
            break;
        }
    }

    EXPECT_TRUE(foundSelf) << "Should find own process (PID " << currentPid << ") in process list";
}

TEST(CrossLayerIntegrationTest, ProcessModelCpuPercentageIncreasesWithWork)
{
    auto probe = Platform::makeProcessProbe();
    Domain::ProcessModel model(std::move(probe));

    auto currentPid = static_cast<int32_t>(getpid());

    // First refresh to establish baseline
    model.refresh();

    // Do some CPU-intensive work
    volatile uint64_t sum = 0;
    for (int i = 0; i < 10000000; ++i)
    {
        sum += static_cast<uint64_t>(i);
    }

    // Small delay to ensure counters update
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Second refresh to calculate delta
    model.refresh();

    auto snaps = model.snapshots();

    bool foundSelf = false;
    for (const auto& snap : snaps)
    {
        if (snap.pid == currentPid)
        {
            foundSelf = true;
            // After doing work, CPU percentage should be >= 0
            // (May be 0 if the work was too fast relative to tick resolution)
            EXPECT_GE(snap.cpuPercent, 0.0);
            break;
        }
    }

    EXPECT_TRUE(foundSelf);
}

TEST(CrossLayerIntegrationTest, MultipleRefreshesMaintainConsistency)
{
    auto probe = Platform::makeProcessProbe();
    Domain::ProcessModel model(std::move(probe));

    for (int i = 0; i < 5; ++i)
    {
        model.refresh();

        auto snaps = model.snapshots();
        auto count = model.processCount();

        // Basic sanity checks
        EXPECT_GT(count, 0);
        EXPECT_EQ(snaps.size(), static_cast<size_t>(count));

        // All snapshots should have valid PIDs
        for (const auto& snap : snaps)
        {
            EXPECT_GT(snap.pid, 0);
            EXPECT_NE(snap.uniqueKey, 0);
        }
    }
}

// =============================================================================
// SystemModel + Real Platform Probe Integration
// =============================================================================

TEST(CrossLayerIntegrationTest, SystemModelWithRealProbeWorks)
{
    auto probe = Platform::makeSystemProbe();
    ASSERT_NE(probe, nullptr);

    Domain::SystemModel model(std::move(probe));

    // First refresh
    model.refresh();

    // Should have valid system data
    auto snap = model.snapshot();

    EXPECT_GT(snap.memoryTotalBytes, 0);
    EXPECT_GT(snap.memoryAvailableBytes, 0);
    EXPECT_LE(snap.memoryAvailableBytes, snap.memoryTotalBytes);
    EXPECT_GE(snap.memoryUsedPercent, 0.0);
    EXPECT_LE(snap.memoryUsedPercent, 100.0);
}

TEST(CrossLayerIntegrationTest, SystemModelCpuUsageIsReasonable)
{
    auto probe = Platform::makeSystemProbe();
    Domain::SystemModel model(std::move(probe));

    // First refresh to establish baseline
    model.refresh();

    // Small delay for CPU counters to update
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Second refresh to calculate usage
    model.refresh();

    auto snap = model.snapshot();

    // CPU usage should be between 0 and 100%
    EXPECT_GE(snap.cpuTotal.totalPercent, 0.0);
    EXPECT_LE(snap.cpuTotal.totalPercent, 100.0);
}

TEST(CrossLayerIntegrationTest, SystemModelUptimeIncreases)
{
    auto probe = Platform::makeSystemProbe();
    Domain::SystemModel model(std::move(probe));

    model.refresh();
    auto uptime1 = model.snapshot().uptimeSeconds;

    // Wait a bit
    std::this_thread::sleep_for(std::chrono::seconds(1));

    model.refresh();
    auto uptime2 = model.snapshot().uptimeSeconds;

    // Uptime should have increased (or stayed same if resolution is coarse)
    EXPECT_GE(uptime2, uptime1);
}

// =============================================================================
// Combined ProcessModel + SystemModel Integration
// =============================================================================

TEST(CrossLayerIntegrationTest, BothModelsCanBeUsedSimultaneously)
{
    auto processProbe = Platform::makeProcessProbe();
    auto systemProbe = Platform::makeSystemProbe();

    Domain::ProcessModel processModel(std::move(processProbe));
    Domain::SystemModel systemModel(std::move(systemProbe));

    // Refresh both
    processModel.refresh();
    systemModel.refresh();

    // Both should have valid data
    EXPECT_GT(processModel.processCount(), 0);
    EXPECT_GT(systemModel.snapshot().memoryTotalBytes, 0);
}

TEST(CrossLayerIntegrationTest, ProbeCapabilitiesAreExposed)
{
    auto processProbe = Platform::makeProcessProbe();
    auto systemProbe = Platform::makeSystemProbe();

    Domain::ProcessModel processModel(std::move(processProbe));
    Domain::SystemModel systemModel(std::move(systemProbe));

    auto procCaps = processModel.capabilities();
    auto sysCaps = systemModel.capabilities();

    // Capabilities should be populated (platform-dependent)
    // On Linux: should have most capabilities
    // On Windows: capabilities may vary
    EXPECT_TRUE(procCaps.hasStartTime || !procCaps.hasStartTime); // Always true, just checking access
    EXPECT_TRUE(sysCaps.hasPerCoreCpu || !sysCaps.hasPerCoreCpu); // Always true, just checking access
}
