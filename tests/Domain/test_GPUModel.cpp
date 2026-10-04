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
#include "Domain/SamplingConfig.h"
#include "Mocks/MockGPUProbe.h"
#include "Platform/GPUTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <thread>

using TestMocks::makeGPUCounters;
using TestMocks::makeGPUInfo;
using TestMocks::MockGPUProbe;

namespace
{

// Bounded wait for a MockGPUProbe's "entered its blocked read" flag. Used by concurrency
// tests that hold the mock blocked from a background thread: without a deadline, a
// regression that stops the mock from ever entering its blocking wait would hang the test
// process indefinitely instead of failing it. Returns false on timeout so the caller can
// report a clear (non-fatal) failure and still run its own cleanup/release/join.
[[nodiscard]] bool waitForBlockedEntry(const MockGPUProbe& probe, std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!probe.hasEnteredBlockedReadGPUCounters())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

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

TEST(GPUModelTest, ReadProcessGPUCountersSkipsProbeWhenCapabilityUnsupported)
{
    // Regression test for #843 Phase 3b: backends that can never return per-process data
    // (e.g. Linux Intel DRM) should never even acquire the probe lock for this call, since
    // that lock is shared with concurrent system-GPU sampling. Verified here by checking the
    // probe's call count directly, not just the (already-empty) return value.
    auto probe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = false;
    probe->withCapabilities(caps);
    probe->withProcessGPU(100, "GPU0", 1024 * 1024);
    auto* rawProbe = probe.get();

    Domain::GPUModel model(std::move(probe));

    const auto counters = model.readProcessGPUCounters();

    EXPECT_TRUE(counters.empty());
    EXPECT_EQ(rawProbe->readProcessCountersCallCount(), 0U);
}

TEST(GPUModelTest, ReadProcessGPUCountersCallsProbeWhenCapabilitySupported)
{
    auto probe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    probe->withCapabilities(caps);
    probe->withProcessGPU(100, "GPU0", 1024 * 1024);
    auto* rawProbe = probe.get();

    Domain::GPUModel model(std::move(probe));

    const auto counters = model.readProcessGPUCounters();

    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].pid, 100);
    EXPECT_EQ(rawProbe->readProcessCountersCallCount(), 1U);
}

// An empty device list is ambiguous on its own: it can mean "looked and found none" or "could not
// look". The publication says which, so the UI does not report a failed enumeration as the absence
// of a GPU (#927 review).
TEST(GPUModelTest, PublicationRecordsSuccessfulEnumerationEvenWhenEmpty)
{
    auto probe = std::make_unique<MockGPUProbe>();

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    const auto publication = model.publication();
    ASSERT_NE(publication, nullptr);
    EXPECT_TRUE(publication->gpuInfoKnown);
    EXPECT_TRUE(publication->gpuInfo.empty());
}

TEST(GPUModelTest, PublicationRecordsSuccessfulEnumerationWithDevices)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    const auto publication = model.publication();
    ASSERT_NE(publication, nullptr);
    EXPECT_TRUE(publication->gpuInfoKnown);
    EXPECT_EQ(publication->gpuInfo.size(), 1U);
}

// The model survives a failed enumeration and keeps publishing; the publication must say the
// device list is not to be trusted.
TEST(GPUModelTest, PublicationRecordsFailedEnumeration)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withEnumerationThrowing();

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    const auto publication = model.publication();
    ASSERT_NE(publication, nullptr);
    EXPECT_FALSE(publication->gpuInfoKnown);
    EXPECT_TRUE(publication->gpuInfo.empty());
}

TEST(GPUModelTest, ReadProcessGPUCountersStillAttemptsProbeWhenCapabilityDiscoveryFailed)
{
    // Regression test for a review finding on #862: the constructor's capabilities() query
    // is wrapped in try/catch, and if it throws, m_Capabilities is left at its default
    // (all-false) values. Treating that as "confirmed unsupported" would permanently and
    // silently suppress a probe that might genuinely support per-process data, just because
    // of a one-time query failure at construction -- a real regression versus this method's
    // behavior before the capability check existed (it always attempted the probe call).
    // readProcessGPUCounters() must fall through to the lock-and-call path when discovery
    // failed (unknown), not treat "unknown" the same as "confirmed false".
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withCapabilitiesQueryThrowingOnce(); // fails only the constructor's one query
    probe->withGPU("GPU0", "Test GPU", "TestVendor");
    probe->withProcessGPU(100, "GPU0", 512ULL * 1024 * 1024);
    auto* rawProbe = probe.get();

    Domain::GPUModel model(std::move(probe)); // constructor's capabilities() query throws here

    const auto counters = model.readProcessGPUCounters();

    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].pid, 100);
    EXPECT_EQ(rawProbe->readProcessCountersCallCount(), 1U);
}

TEST(GPUModelTest, ReadProcessGPUCountersDoesNotWaitBehindProbeLockWhenUnsupported)
{
    // ReadProcessGPUCountersSkipsProbeWhenCapabilityUnsupported above only proves the probe
    // method is skipped -- it would still pass if readProcessGPUCounters() acquired
    // m_ProbeMutex and checked the capability afterward, since the probe call it's counting
    // happens inside a different method (readGPUCounters(), called by refresh()). The actual
    // regression being fixed is lock CONTENTION: readProcessGPUCounters() must return without
    // ever waiting on m_ProbeMutex when unsupported, even while another thread holds that
    // mutex doing a (slow) refresh(). Prove that directly: hold the probe lock open via a
    // background refresh() blocked inside the mock, then confirm the unsupported call
    // completes promptly instead of waiting for it to be released.
    auto probe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = false;
    probe->withCapabilities(caps);
    probe->withGPU("GPU0", "Test GPU", "TestVendor");
    auto* rawProbe = probe.get();
    rawProbe->armBlockingReadGPUCounters();

    Domain::GPUModel model(std::move(probe));

    std::thread blockedRefresh([&model] { model.refresh(); });

    // Wait until the background refresh() is actually inside the blocked probe call (and
    // therefore holding m_ProbeMutex), not just scheduled -- otherwise the timing check
    // below could race ahead of the lock actually being held. Bounded (not a bare spin loop):
    // if a regression stopped refresh() from ever reaching the mock's blocking point, this
    // would otherwise hang the test process forever instead of failing it. Non-fatal so the
    // release()/join() cleanup below still runs regardless.
    EXPECT_TRUE(waitForBlockedEntry(*rawProbe)) << "background refresh() never entered its blocking probe call within the deadline "
                                                   "-- GPUModel::refresh() or the mock may be broken";

    // Run the call under test on its own thread with a bounded wait, rather than calling it
    // inline: if the fix regressed and this call actually blocked on m_ProbeMutex, an inline
    // call would hang the test binary forever (the lock is only released below, after this
    // check). A bounded wait_for lets that scenario fail with a clear assertion instead.
    auto future = std::async(std::launch::async, [&model] { return model.readProcessGPUCounters(); });
    const auto status = future.wait_for(std::chrono::milliseconds(500));

    EXPECT_EQ(status, std::future_status::ready)
        << "readProcessGPUCounters() did not return promptly -- it appears to be waiting on the probe lock";

    rawProbe->releaseBlockedReadGPUCounters();
    blockedRefresh.join();

    if (status == std::future_status::ready)
    {
        EXPECT_TRUE(future.get().empty());
    }
    else
    {
        // Let the async task finish before the test function returns and its captures
        // (model, rawProbe) go out of scope, even though the assertion above already failed.
        future.wait();
    }
}

TEST(MockGPUProbeTest, ArmBlockingReadGPUCountersResetsStateFromPriorReleaseCycle)
{
    // Regression test for a review finding on #862: releaseBlockedReadGPUCounters() left
    // m_ReleaseRequested and the "entered" flag set to true, so re-arming the mock without
    // resetting them made the next blocked call's wait predicate succeed immediately (never
    // actually blocking) and made hasEnteredBlockedReadGPUCounters() report true before the
    // new block even started. Exercise two full arm/release cycles on the same mock and
    // confirm the second one genuinely blocks and genuinely reports "entered" only once it
    // has, not from stale state left over by the first cycle.
    MockGPUProbe probe;

    for (int cycle = 0; cycle < 2; ++cycle)
    {
        probe.armBlockingReadGPUCounters();
        ASSERT_FALSE(probe.hasEnteredBlockedReadGPUCounters())
            << "cycle " << cycle << ": entered flag should start false immediately after arming";

        auto future = std::async(std::launch::async, [&probe] { return probe.readGPUCounters(); });

        // Bounded (not a bare spin loop): if a regression stopped readGPUCounters() from
        // ever entering its blocking wait, this would otherwise hang the test process
        // forever instead of failing it. Non-fatal so the release()/wait() cleanup below
        // still runs regardless, unblocking the async call either way.
        EXPECT_TRUE(waitForBlockedEntry(probe))
            << "cycle " << cycle << ": readGPUCounters() never entered its blocking wait within the deadline";

        // The call must still be genuinely blocked at this point, not already completed.
        const auto status = future.wait_for(std::chrono::milliseconds(50));
        EXPECT_EQ(status, std::future_status::timeout) << "cycle " << cycle << ": call returned before being released";

        probe.releaseBlockedReadGPUCounters();
        future.wait();
    }
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

TEST(GPUModelTest, SampleDelegatesToRefresh)
{
    // sample() is the ISamplable override BackgroundSampler calls in production; every other
    // test in this file drives refresh() directly, so this is the only coverage of sample()'s
    // own body.
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    static_cast<Domain::ISamplable&>(model).sample();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].gpuId, "GPU0");
}

TEST(GPUModelTest, PublishesCoherentVersionedState)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 75.0);

    Domain::GPUModel model(std::move(probe));
    model.refresh();
    const auto first = model.publication();
    model.refresh();
    const auto second = model.publication();

    EXPECT_EQ(first->version, 1);
    EXPECT_EQ(second->version, 2);
    ASSERT_EQ(second->snapshots.size(), 1);
    const auto historyIt = second->histories.find("GPU0");
    ASSERT_NE(historyIt, second->histories.end());
    EXPECT_EQ(historyIt->second.timestamps.size(), historyIt->second.utilization.size());
    EXPECT_EQ(historyIt->second.memoryUsedBytes.size(), historyIt->second.timestamps.size());
    EXPECT_EQ(historyIt->second.memoryTotalBytes.size(), historyIt->second.timestamps.size());
}

TEST(GPUModelTest, PublishesHistoryForGpuDiscoveredAfterConstruction)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto counters = makeGPUCounters("GPU-late");
    counters.utilizationPercent = 67.0;
    probe->withGPUCounters("GPU-late", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    const auto publication = model.publication();
    ASSERT_EQ(publication->snapshots.size(), 1);
    EXPECT_EQ(publication->snapshots[0].gpuId, "GPU-late");

    const auto historyIt = publication->histories.find("GPU-late");
    ASSERT_NE(historyIt, publication->histories.end());
    ASSERT_EQ(historyIt->second.utilization.size(), 1);
    EXPECT_FLOAT_EQ(historyIt->second.utilization[0], 67.0F);
    EXPECT_EQ(historyIt->second.timestamps.size(), 1);
    EXPECT_EQ(historyIt->second.utilization.size(), 1);
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

TEST(GPUModelTest, FanSpeedPercentIsComputedFromRawAndMax)
{
    // Mirrors a ROCm-style probe: a raw sensor reading normalized against a device-reported
    // max that isn't a round number, so the division isn't exact (see #734).
    auto probe = std::make_unique<MockGPUProbe>();
    auto counters = makeGPUCounters("GPU0");
    counters.fanSpeedRaw = 170;
    counters.fanSpeedMaxRaw = 255;
    probe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // 170 * 100 / 255 = 66 (integer division)
    EXPECT_EQ(snaps[0].fanSpeedPercent, 66U);
    EXPECT_TRUE(snaps[0].fanSpeedAvailable);
}

TEST(GPUModelTest, FanSpeedPercentIsNotClampedWhenRawExceedsMax)
{
    // Left unclamped, matching memoryUsedPercent/powerUtilPercent: a raw reading above the
    // device's reported max (sensor drift, transient overspeed) is itself useful signal, not
    // something Domain should silently cap.
    auto probe = std::make_unique<MockGPUProbe>();
    auto counters = makeGPUCounters("GPU0");
    counters.fanSpeedRaw = 300;
    counters.fanSpeedMaxRaw = 255;
    probe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // 300 * 100 / 255 = 117 (integer division)
    EXPECT_EQ(snaps[0].fanSpeedPercent, 117U);
    EXPECT_TRUE(snaps[0].fanSpeedAvailable);
}

TEST(GPUModelTest, FanSpeedPercentIsZeroAndUnavailableWhenMaxUnavailable)
{
    // fanSpeedMaxRaw == 0 means the probe couldn't determine a max this poll (e.g. ROCm's
    // optional rsmi_dev_fan_speed_max_get symbol wasn't loaded, or a transient call failure).
    // Domain must not divide by zero, and must mark the sample as unavailable rather than
    // reporting a percentage indistinguishable from a genuine 0% reading.
    auto probe = std::make_unique<MockGPUProbe>();
    auto counters = makeGPUCounters("GPU0");
    counters.fanSpeedRaw = 170;
    counters.fanSpeedMaxRaw = 0;
    probe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    EXPECT_EQ(snaps[0].fanSpeedPercent, 0U);
    EXPECT_FALSE(snaps[0].fanSpeedAvailable);
}

TEST(GPUModelTest, FanSpeedHistoryMarksUnavailableSamplesAsNaN)
{
    // An unavailable sample must come back as NaN, not 0.0F, from both the published history
    // (what GpuSection's chart/tooltip actually consume) and the fanSpeedHistory() accessor -
    // otherwise a caller can't tell "fan genuinely at 0%" from "couldn't read the fan this poll".
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    auto available = makeGPUCounters("GPU0");
    available.fanSpeedRaw = 170;
    available.fanSpeedMaxRaw = 255;
    probe->withGPUCounters("GPU0", available);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto unavailable = makeGPUCounters("GPU0");
    unavailable.fanSpeedRaw = 170;
    unavailable.fanSpeedMaxRaw = 0;
    // A second, independent model+probe for the unavailable case: MockGPUProbe's counters are
    // fixed at construction, and it was already moved into the first model above.
    auto secondProbe = std::make_unique<MockGPUProbe>();
    secondProbe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", unavailable);
    Domain::GPUModel unavailableModel(std::move(secondProbe));
    unavailableModel.refresh();

    const auto publication = unavailableModel.publication();
    const auto historyIt = publication->histories.find("GPU0");
    ASSERT_NE(historyIt, publication->histories.end());
    ASSERT_EQ(historyIt->second.fanSpeed.size(), 1);
    EXPECT_TRUE(std::isnan(historyIt->second.fanSpeed[0]));

    const auto fanHistory = unavailableModel.fanSpeedHistory("GPU0");
    ASSERT_EQ(fanHistory.size(), 1);
    EXPECT_TRUE(std::isnan(fanHistory[0]));

    // Sanity check the available-sample sibling model is NOT NaN, so this test would actually
    // fail if fanSpeedAvailable stopped being honored.
    const auto availableFanHistory = model.fanSpeedHistory("GPU0");
    ASSERT_EQ(availableFanHistory.size(), 1);
    EXPECT_FALSE(std::isnan(availableFanHistory[0]));
    EXPECT_FLOAT_EQ(availableFanHistory[0], 66.0F);
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

    // Both rates use the same measured interval, so their ratio is deterministic
    // even when a loaded CI runner delays the second refresh.
    EXPECT_NEAR(snaps[0].pcieRxBytesPerSec, snaps[0].pcieTxBytesPerSec * 2.0, snaps[0].pcieTxBytesPerSec * 0.001);
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
    auto hist0 = model.history("GPU0");
    auto hist1 = model.history("GPU1");

    EXPECT_EQ(hist0.size(), 2);
    EXPECT_EQ(hist1.size(), 2);

    // Verify latest values (last element in vector)
    EXPECT_DOUBLE_EQ(hist0.back().utilizationPercent, 60.0);
    EXPECT_DOUBLE_EQ(hist1.back().utilizationPercent, 80.0);
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

    auto hist = model.history("NonexistentGPU");
    EXPECT_EQ(hist.size(), 0);
}

// =============================================================================
// History Accessor Tests (For Chart Plotting)
// =============================================================================

TEST(GPUModelTest, UtilizationHistoryReturnsCorrectValues)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 50.0);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    // Update utilization and refresh again
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    rawProbe->withUtilization("GPU0", 75.0);
    model.refresh();

    auto hist = model.utilizationHistory("GPU0");
    ASSERT_EQ(hist.size(), 2);
    EXPECT_FLOAT_EQ(hist[0], 50.0F);
    EXPECT_FLOAT_EQ(hist[1], 75.0F);
}

TEST(GPUModelTest, UtilizationHistoryReturnsEmptyForNonexistentGPU)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto hist = model.utilizationHistory("NonexistentGPU");
    EXPECT_TRUE(hist.empty());
}

TEST(GPUModelTest, MemoryPercentHistoryReturnsCorrectValues)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    // 2GB used / 8GB total = 25%
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withMemory("GPU0", 2ULL * 1024 * 1024 * 1024, 8ULL * 1024 * 1024 * 1024);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    // Update memory and refresh
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // 4GB used / 8GB total = 50%
    rawProbe->withMemory("GPU0", 4ULL * 1024 * 1024 * 1024, 8ULL * 1024 * 1024 * 1024);
    model.refresh();

    auto hist = model.memoryPercentHistory("GPU0");
    ASSERT_EQ(hist.size(), 2);
    EXPECT_FLOAT_EQ(hist[0], 25.0F);
    EXPECT_FLOAT_EQ(hist[1], 50.0F);
}

TEST(GPUModelTest, MemoryPercentHistoryReturnsEmptyForNonexistentGPU)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto hist = model.memoryPercentHistory("NonexistentGPU");
    EXPECT_TRUE(hist.empty());
}

TEST(GPUModelTest, GpuClockHistoryReturnsCorrectValues)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    auto counters = makeGPUCounters("GPU0");
    counters.gpuClockMHz = 1200;
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    // Update clock and refresh
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    counters.gpuClockMHz = 1800;
    rawProbe->withGPUCounters("GPU0", counters);
    model.refresh();

    auto hist = model.gpuClockHistory("GPU0");
    ASSERT_EQ(hist.size(), 2);
    EXPECT_FLOAT_EQ(hist[0], 1200.0F);
    EXPECT_FLOAT_EQ(hist[1], 1800.0F);
}

TEST(GPUModelTest, GpuClockHistoryReturnsEmptyForNonexistentGPU)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto hist = model.gpuClockHistory("NonexistentGPU");
    EXPECT_TRUE(hist.empty());
}

TEST(GPUModelTest, EncoderHistoryReturnsCorrectValues)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    auto counters = makeGPUCounters("GPU0");
    counters.encoderUtilPercent = 30.0;
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    // Update encoder and refresh
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    counters.encoderUtilPercent = 85.0;
    rawProbe->withGPUCounters("GPU0", counters);
    model.refresh();

    auto hist = model.encoderHistory("GPU0");
    ASSERT_EQ(hist.size(), 2);
    EXPECT_FLOAT_EQ(hist[0], 30.0F);
    EXPECT_FLOAT_EQ(hist[1], 85.0F);
}

TEST(GPUModelTest, EncoderHistoryReturnsEmptyForNonexistentGPU)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto hist = model.encoderHistory("NonexistentGPU");
    EXPECT_TRUE(hist.empty());
}

TEST(GPUModelTest, DecoderHistoryReturnsCorrectValues)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    auto counters = makeGPUCounters("GPU0");
    counters.decoderUtilPercent = 15.0;
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    // Update decoder and refresh
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    counters.decoderUtilPercent = 60.0;
    rawProbe->withGPUCounters("GPU0", counters);
    model.refresh();

    auto hist = model.decoderHistory("GPU0");
    ASSERT_EQ(hist.size(), 2);
    EXPECT_FLOAT_EQ(hist[0], 15.0F);
    EXPECT_FLOAT_EQ(hist[1], 60.0F);
}

TEST(GPUModelTest, DecoderHistoryReturnsEmptyForNonexistentGPU)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto hist = model.decoderHistory("NonexistentGPU");
    EXPECT_TRUE(hist.empty());
}

TEST(GPUModelTest, TemperatureHistoryReturnsCorrectValues)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    auto counters = makeGPUCounters("GPU0");
    counters.temperatureC = 45;
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    // Update temperature and refresh
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    counters.temperatureC = 72;
    rawProbe->withGPUCounters("GPU0", counters);
    model.refresh();

    auto hist = model.temperatureHistory("GPU0");
    ASSERT_EQ(hist.size(), 2);
    EXPECT_FLOAT_EQ(hist[0], 45.0F);
    EXPECT_FLOAT_EQ(hist[1], 72.0F);
}

TEST(GPUModelTest, TemperatureHistoryReturnsEmptyForNonexistentGPU)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto hist = model.temperatureHistory("NonexistentGPU");
    EXPECT_TRUE(hist.empty());
}

TEST(GPUModelTest, PowerHistoryReturnsCorrectValues)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    auto counters = makeGPUCounters("GPU0");
    counters.powerDrawWatts = 120.5;
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    // Update power and refresh
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    counters.powerDrawWatts = 250.0;
    rawProbe->withGPUCounters("GPU0", counters);
    model.refresh();

    auto hist = model.powerHistory("GPU0");
    ASSERT_EQ(hist.size(), 2);
    EXPECT_FLOAT_EQ(hist[0], 120.5F);
    EXPECT_FLOAT_EQ(hist[1], 250.0F);
}

TEST(GPUModelTest, PowerHistoryReturnsEmptyForNonexistentGPU)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto hist = model.powerHistory("NonexistentGPU");
    EXPECT_TRUE(hist.empty());
}

TEST(GPUModelTest, FanSpeedHistoryReturnsCorrectValues)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    auto counters = makeGPUCounters("GPU0");
    counters.fanSpeedRaw = 40;
    counters.fanSpeedMaxRaw = 100;
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    // Update fan speed and refresh
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    counters.fanSpeedRaw = 95;
    counters.fanSpeedMaxRaw = 100;
    rawProbe->withGPUCounters("GPU0", counters);
    model.refresh();

    auto hist = model.fanSpeedHistory("GPU0");
    ASSERT_EQ(hist.size(), 2);
    EXPECT_FLOAT_EQ(hist[0], 40.0F);
    EXPECT_FLOAT_EQ(hist[1], 95.0F);
}

TEST(GPUModelTest, FanSpeedHistoryReturnsEmptyForNonexistentGPU)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto hist = model.fanSpeedHistory("NonexistentGPU");
    EXPECT_TRUE(hist.empty());
}

// =============================================================================
// History Timestamps Tests
// =============================================================================

TEST(GPUModelTest, HistoryTimestampsAlignWithHistoryData)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 50.0);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    // Refresh a few more times
    for (int i = 0; i < 3; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        rawProbe->withUtilization("GPU0", 50.0 + static_cast<double>(i) * 10.0);
        model.refresh();
    }

    auto timestamps = model.historyTimestamps();
    auto utilHist = model.utilizationHistory("GPU0");

    // Timestamps should match history length
    EXPECT_EQ(timestamps.size(), utilHist.size());
    EXPECT_EQ(timestamps.size(), 4); // Initial + 3 updates

    // Timestamps should be monotonically increasing
    for (size_t i = 1; i < timestamps.size(); ++i)
    {
        EXPECT_GT(timestamps[i], timestamps[i - 1]);
    }
}

TEST(GPUModelTest, HistoryTimestampsEmptyWhenNoRefresh)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    // No refresh called

    auto timestamps = model.historyTimestamps();
    EXPECT_TRUE(timestamps.empty());
}

// =============================================================================
// Per-GPU historyTimestamps(gpuId) Tests
// =============================================================================

TEST(GPUModelTest, PerGpuHistoryTimestampsEmptyForUnknownGpu)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    EXPECT_TRUE(model.historyTimestamps("GPU_UNKNOWN").empty());
}

TEST(GPUModelTest, PerGpuHistoryTimestampsEmptyWhenNoRefresh)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    // No refresh called

    EXPECT_TRUE(model.historyTimestamps("GPU0").empty());
}

TEST(GPUModelTest, PerGpuHistoryTimestampsLengthMatchesHistory)
{
    // historyTimestamps(gpuId) must return the same number of entries
    // as the per-GPU utilization history so indices stay aligned.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 10.0);

    Domain::GPUModel model(std::move(probe));
    for (int i = 0; i < 4; ++i)
    {
        rawProbe->withUtilization("GPU0", static_cast<double>(i) * 10.0);
        model.refresh();
    }

    const auto timestamps = model.historyTimestamps("GPU0");
    const auto utilHist = model.utilizationHistory("GPU0");
    EXPECT_EQ(timestamps.size(), utilHist.size());
    EXPECT_EQ(timestamps.size(), 4u);
}

TEST(GPUModelTest, PerGpuHistoryTimestampsAreMonotonicallyIncreasing)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 0.0);

    Domain::GPUModel model(std::move(probe));
    for (int i = 0; i < 3; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        rawProbe->withUtilization("GPU0", static_cast<double>(i) * 20.0);
        model.refresh();
    }

    const auto timestamps = model.historyTimestamps("GPU0");
    ASSERT_GE(timestamps.size(), 2u);
    for (std::size_t i = 1; i < timestamps.size(); ++i)
    {
        EXPECT_GT(timestamps[i], timestamps[i - 1]);
    }
}

TEST(GPUModelTest, PerGpuHistoryTimestampsIndexAlignedWithSnapshotAt)
{
    // Verify that historyTimestamps("GPU0")[i] matches snapshotAt("GPU0", i).captureTimeSec.
    // This is the alignment property that GpuSection relies on for tooltip timestamps.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 0.0);

    Domain::GPUModel model(std::move(probe));
    constexpr int kSamples = 5;
    for (int i = 0; i < kSamples; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        rawProbe->withUtilization("GPU0", static_cast<double>(i) * 10.0);
        model.refresh();
    }

    const auto timestamps = model.historyTimestamps("GPU0");
    ASSERT_EQ(static_cast<int>(timestamps.size()), kSamples);
    for (std::size_t i = 0; i < timestamps.size(); ++i)
    {
        const auto snap = model.snapshotAt("GPU0", i);
        ASSERT_TRUE(snap.has_value());
        EXPECT_DOUBLE_EQ(timestamps[i], snap->captureTimeSec);
    }
}

TEST(GPUModelTest, PerGpuHistoryTimestampsCappedAtCapacity)
{
    // After pushing more samples than the ring holds (all at once, so the time window
    // trims nothing), historyTimestamps(gpuId) returns exactly the capacity.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MIN);
    const std::size_t capacity = Domain::Sampling::historyCapacityForSeconds(Domain::Sampling::HISTORY_SECONDS_MIN);
    const std::size_t overCapacity = capacity + 5;
    for (std::size_t i = 0; i < overCapacity; ++i)
    {
        rawProbe->withUtilization("GPU0", static_cast<double>(i));
        model.refresh();
    }

    EXPECT_EQ(model.historyTimestamps("GPU0").size(), capacity);
}

TEST(GPUModelTest, PerGpuHistoryTimestampsIndependentPerGpu)
{
    // Two GPUs accumulate their own per-GPU timestamp vectors; they must have
    // the same length as their own history even when one GPU has fewer samples.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "GPU Zero", "VendorA").withGPU("GPU1", "GPU One", "VendorB");

    Domain::GPUModel model(std::move(probe));
    for (int i = 0; i < 3; ++i)
    {
        rawProbe->withUtilization("GPU0", 10.0).withUtilization("GPU1", 20.0);
        model.refresh();
    }

    const auto ts0 = model.historyTimestamps("GPU0");
    const auto ts1 = model.historyTimestamps("GPU1");
    EXPECT_EQ(ts0.size(), 3u);
    EXPECT_EQ(ts1.size(), 3u);

    // Each set of timestamps must match its own history length.
    EXPECT_EQ(ts0.size(), model.utilizationHistory("GPU0").size());
    EXPECT_EQ(ts1.size(), model.utilizationHistory("GPU1").size());
}

TEST(GPUModelTest, FailedSensorReadsPublishGapsNotZeros)
{
    // #1111: a sample whose utilization, temperature, power or clock read failed publishes NaN for
    // that field -- a gap -- not the counter's 0, while fields that were read stay real.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");
    Platform::GPUCounters good;
    good.gpuId = "GPU0";
    good.utilizationPercent = 40.0;
    good.temperatureC = 60;
    good.powerDrawWatts = 90.0;
    good.gpuClockMHz = 1500;
    rawProbe->withGPUCounters("GPU0", good);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    Platform::GPUCounters failed = good;
    failed.utilizationAvailable = false;
    failed.temperatureAvailable = false;
    failed.powerAvailable = false;
    failed.gpuClockAvailable = false;
    failed.utilizationPercent = 0.0;
    failed.temperatureC = 0;
    failed.powerDrawWatts = 0.0;
    failed.gpuClockMHz = 0;
    rawProbe->withGPUCounters("GPU0", failed);
    model.refresh();

    for (const auto& series :
         {model.utilizationHistory("GPU0"), model.temperatureHistory("GPU0"), model.powerHistory("GPU0"), model.gpuClockHistory("GPU0")})
    {
        ASSERT_EQ(series.size(), 2U);
        EXPECT_FALSE(std::isnan(series[0]));
        EXPECT_TRUE(std::isnan(series[1]));
    }

    const auto publication = model.publication();
    const auto& published = publication->histories.at("GPU0");
    EXPECT_TRUE(std::isnan(published.utilization.back()));
    EXPECT_TRUE(std::isnan(published.temperature.back()));
    EXPECT_TRUE(std::isnan(published.power.back()));
    EXPECT_TRUE(std::isnan(published.gpuClock.back()));
    EXPECT_FLOAT_EQ(published.utilization.front(), 40.0F);

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_FALSE(snaps[0].utilizationAvailable);
    EXPECT_FALSE(snaps[0].temperatureAvailable);
}

TEST(GPUModelTest, PerGpuHistoryHasAGapWhileGpuAbsent)
{
    // A GPU missing from a refresh gets a placeholder in its own history, published as NaN, so the
    // chart shows a gap across the absence instead of a line joining the samples either side of it
    // (#1146). Its timestamps stay aligned with its history vectors and with the global timestamps.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 10.0);

    Domain::GPUModel model(std::move(probe));

    rawProbe->withUtilization("GPU0", 20.0);
    model.refresh();
    rawProbe->withUtilization("GPU0", 30.0);
    model.refresh();

    rawProbe->clearGPUs(); // GPU0 missing from this read
    model.refresh();

    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 50.0);
    model.refresh();

    const auto globalTs = model.historyTimestamps();
    const auto perGpuTs = model.historyTimestamps("GPU0");
    ASSERT_EQ(globalTs.size(), 4U);
    ASSERT_EQ(perGpuTs.size(), 4U);
    for (std::size_t i = 0; i < globalTs.size(); ++i)
    {
        EXPECT_DOUBLE_EQ(perGpuTs[i], globalTs[i]);
    }

    const auto utilHist = model.utilizationHistory("GPU0");
    ASSERT_EQ(utilHist.size(), 4U);
    EXPECT_FLOAT_EQ(utilHist[1], 30.0F);
    EXPECT_TRUE(std::isnan(utilHist[2]));
    EXPECT_FLOAT_EQ(utilHist[3], 50.0F);
    EXPECT_TRUE(std::isnan(model.temperatureHistory("GPU0")[2]));

    const auto publication = model.publication();
    const auto historyIt = publication->histories.find("GPU0");
    ASSERT_NE(historyIt, publication->histories.end());
    const auto& published = historyIt->second;
    ASSERT_EQ(published.timestamps.size(), 4U);
    for (const auto* series : {&published.utilization,
                               &published.memoryPercent,
                               &published.gpuClock,
                               &published.encoder,
                               &published.decoder,
                               &published.temperature,
                               &published.power,
                               &published.fanSpeed})
    {
        ASSERT_EQ(series->size(), 4U);
        EXPECT_TRUE(std::isnan((*series)[2]));
    }
    EXPECT_FALSE(std::isnan(published.temperature[3]));
}

TEST(GPUModelTest, GpuMissingForTheWholeWindowIsForgotten)
{
    // Placeholders are recorded only while some real sample is still in the window; after that the
    // GPU's history is dropped rather than padded with NaN forever (#1146).
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 10.0);

    Domain::GPUModel model(std::move(probe));
    model.setMaxHistorySeconds(10.0);

    const auto start = std::chrono::ceil<std::chrono::seconds>(std::chrono::steady_clock::now());
    model.refreshAt(start);
    rawProbe->clearGPUs();
    for (int i = 1; i <= 5; ++i)
    {
        model.refreshAt(start + std::chrono::seconds(i));
    }
    EXPECT_EQ(model.historyTimestamps("GPU0").size(), 6U); // Still in the window: one sample, five gaps

    for (int i = 6; i <= 30; ++i)
    {
        model.refreshAt(start + std::chrono::seconds(i));
    }
    EXPECT_TRUE(model.historyTimestamps("GPU0").empty());
    EXPECT_FALSE(model.publication()->histories.contains("GPU0"));
}

// =============================================================================
// Multi-GPU History Tests
// =============================================================================

TEST(GPUModelTest, HistoryAccessorsWorkWithMultipleGPUs)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "GPU Zero", "VendorA")
        .withUtilization("GPU0", 25.0)
        .withGPU("GPU1", "GPU One", "VendorB")
        .withUtilization("GPU1", 75.0);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    auto hist0 = model.utilizationHistory("GPU0");
    auto hist1 = model.utilizationHistory("GPU1");

    ASSERT_EQ(hist0.size(), 1);
    ASSERT_EQ(hist1.size(), 1);
    EXPECT_FLOAT_EQ(hist0[0], 25.0F);
    EXPECT_FLOAT_EQ(hist1[0], 75.0F);
}

// =============================================================================
// Thread Safety Tests for History Accessors
// =============================================================================

TEST(GPUModelTest, ConcurrentHistoryAccessDuringRefresh)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 50.0);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> readCount{0};
    std::atomic<bool> hadError{false};

    // Reader thread that calls all history accessors
    // Note: Each accessor call is independent, so we cannot guarantee they see
    // the same snapshot. We verify they don't crash and return valid data.
    auto reader = std::jthread(
        [&](std::stop_token st)
        {
            while (!stop.load() && !st.stop_requested())
            {
                // Each call should return valid data without crashing
                auto utilHist = model.utilizationHistory("GPU0");
                auto memHist = model.memoryPercentHistory("GPU0");
                auto clockHist = model.gpuClockHistory("GPU0");
                auto encHist = model.encoderHistory("GPU0");
                auto decHist = model.decoderHistory("GPU0");
                auto tempHist = model.temperatureHistory("GPU0");
                auto powerHist = model.powerHistory("GPU0");
                auto fanHist = model.fanSpeedHistory("GPU0");
                auto timestamps = model.historyTimestamps();

                // Verify we got non-empty results (at least one refresh happened)
                if (utilHist.empty() || timestamps.empty())
                {
                    hadError.store(true);
                }

                ++readCount;
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });

    // Perform several refreshes
    for (int i = 0; i < 10; ++i)
    {
        rawProbe->withUtilization("GPU0", 50.0 + static_cast<double>(i));
        model.refresh();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop.store(true);
    reader.join();

    // Verify readers executed without errors
    EXPECT_GT(readCount.load(), 0);
    EXPECT_FALSE(hadError.load());
}

// =============================================================================
// Per-Process GPU Counter Tests
// =============================================================================

TEST(GPUModelTest, ReadProcessGPUCountersReturnsProbeData)
{
    auto probe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    probe->withCapabilities(caps);
    probe->withGPU("GPU0", "Test GPU", "TestVendor")
        .withProcessGPU(1234, "GPU0", 512ULL * 1024 * 1024)
        .withProcessGPU(5678, "GPU0", 256ULL * 1024 * 1024);

    Domain::GPUModel model(std::move(probe));

    auto counters = model.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 2);

    // Verify first process
    auto it1 = std::find_if(counters.begin(), counters.end(), [](const auto& c) { return c.pid == 1234; });
    ASSERT_NE(it1, counters.end());
    EXPECT_EQ(it1->gpuId, "GPU0");
    EXPECT_EQ(it1->gpuMemoryBytes, 512ULL * 1024 * 1024);

    // Verify second process
    auto it2 = std::find_if(counters.begin(), counters.end(), [](const auto& c) { return c.pid == 5678; });
    ASSERT_NE(it2, counters.end());
    EXPECT_EQ(it2->gpuId, "GPU0");
    EXPECT_EQ(it2->gpuMemoryBytes, 256ULL * 1024 * 1024);
}

TEST(GPUModelTest, ReadProcessGPUCountersMultiGPU)
{
    auto probe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    probe->withCapabilities(caps);
    probe->withGPU("GPU0", "GPU 0", "Vendor")
        .withGPU("GPU1", "GPU 1", "Vendor")
        .withProcessGPU(1000, "GPU0", 100ULL * 1024 * 1024)
        .withProcessGPU(1000, "GPU1", 200ULL * 1024 * 1024) // Same process, different GPU
        .withProcessGPU(2000, "GPU1", 300ULL * 1024 * 1024);

    Domain::GPUModel model(std::move(probe));

    auto counters = model.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 3);

    // Find entries for PID 1000
    int pid1000Count = 0;
    for (const auto& c : counters)
    {
        if (c.pid == 1000)
        {
            ++pid1000Count;
        }
    }
    EXPECT_EQ(pid1000Count, 2);

    // Find entry for PID 2000
    auto it = std::find_if(counters.begin(), counters.end(), [](const auto& c) { return c.pid == 2000; });
    ASSERT_NE(it, counters.end());
    EXPECT_EQ(it->gpuId, "GPU1");
}

TEST(GPUModelTest, ReadProcessGPUCountersWithNullProbe)
{
    Domain::GPUModel model(nullptr);

    auto counters = model.readProcessGPUCounters();
    EXPECT_TRUE(counters.empty());
}

TEST(GPUModelTest, ReadProcessGPUCountersCallCountTracked)
{
    auto probe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    probe->withCapabilities(caps);
    // rawProbe remains valid after std::move(probe) because GPUModel stores the unique_ptr
    auto* rawProbe = probe.get();
    probe->withGPU("GPU0", "Test GPU", "Vendor").withProcessGPU(100, "GPU0", 50ULL * 1024 * 1024);

    Domain::GPUModel model(std::move(probe));

    EXPECT_EQ(rawProbe->readProcessCountersCallCount(), 0);

    auto counters1 = model.readProcessGPUCounters();
    EXPECT_EQ(rawProbe->readProcessCountersCallCount(), 1);
    EXPECT_EQ(counters1.size(), 1);

    auto counters2 = model.readProcessGPUCounters();
    EXPECT_EQ(rawProbe->readProcessCountersCallCount(), 2);
    EXPECT_EQ(counters2.size(), 1);
}

// =============================================================================
// snapshotAt Tests
// =============================================================================

TEST(GPUModelTest, SnapshotAtReturnsNulloptForUnknownGPU)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    EXPECT_FALSE(model.snapshotAt("GPU_UNKNOWN", 0).has_value());
}

TEST(GPUModelTest, SnapshotAtReturnsNulloptForOutOfRangeIndex)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 42.0);

    Domain::GPUModel model(std::move(probe));
    model.refresh(); // 1 sample

    EXPECT_FALSE(model.snapshotAt("GPU0", 1).has_value()); // index 1 is out of range
    EXPECT_FALSE(model.snapshotAt("GPU0", 999).has_value());
}

TEST(GPUModelTest, SnapshotAtReturnsNulloptWhenNoRefresh)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    // No refresh called

    EXPECT_FALSE(model.snapshotAt("GPU0", 0).has_value());
}

TEST(GPUModelTest, SnapshotAtReturnsCorrectSampleByIndex)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 10.0);

    Domain::GPUModel model(std::move(probe));
    model.refresh(); // index 0: util=10

    rawProbe->withUtilization("GPU0", 20.0);
    model.refresh(); // index 1: util=20

    rawProbe->withUtilization("GPU0", 30.0);
    model.refresh(); // index 2: util=30

    auto s0 = model.snapshotAt("GPU0", 0);
    auto s1 = model.snapshotAt("GPU0", 1);
    auto s2 = model.snapshotAt("GPU0", 2);

    ASSERT_TRUE(s0.has_value());
    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s2.has_value());

    EXPECT_DOUBLE_EQ(s0->utilizationPercent, 10.0);
    EXPECT_DOUBLE_EQ(s1->utilizationPercent, 20.0);
    EXPECT_DOUBLE_EQ(s2->utilizationPercent, 30.0);
}

TEST(GPUModelTest, SnapshotAtReturnsValueMatchingHistoryByIndex)
{
    // Verify snapshotAt returns the same value as history() at the same logical index.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 50.0);

    Domain::GPUModel model(std::move(probe));
    for (int i = 0; i < 5; ++i)
    {
        rawProbe->withUtilization("GPU0", static_cast<double>(i) * 10.0);
        model.refresh();
    }

    auto fullHistory = model.history("GPU0");
    ASSERT_EQ(fullHistory.size(), 5);

    for (std::size_t i = 0; i < 5; ++i)
    {
        auto snap = model.snapshotAt("GPU0", i);
        ASSERT_TRUE(snap.has_value());
        EXPECT_DOUBLE_EQ(snap->utilizationPercent, fullHistory[i].utilizationPercent);
    }
}

TEST(GPUModelTest, SnapshotAtWrapsAroundAfterCapacityExceeded)
{
    // Push more samples than the ring's capacity to verify the ring-buffer
    // wraparound: index 0 should return the oldest *retained* sample, not the
    // original first sample, and the last index should return the newest sample.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MIN);
    const std::size_t capacity = Domain::Sampling::historyCapacityForSeconds(Domain::Sampling::HISTORY_SECONDS_MIN);

    const std::size_t overCapacity = capacity + 10;
    for (std::size_t i = 0; i < overCapacity; ++i)
    {
        rawProbe->withUtilization("GPU0", static_cast<double>(i));
        model.refresh();
    }

    // History should be capped at the capacity, not overCapacity.
    auto fullHistory = model.history("GPU0");
    ASSERT_EQ(fullHistory.size(), capacity);

    // Index 0 is the oldest retained sample (value = overCapacity - capacity).
    const auto expectedOldestUtil = static_cast<double>(overCapacity - capacity);
    auto s0 = model.snapshotAt("GPU0", 0);
    ASSERT_TRUE(s0.has_value());
    EXPECT_DOUBLE_EQ(s0->utilizationPercent, expectedOldestUtil);

    // Last index is the newest sample (value = overCapacity - 1).
    const auto expectedNewestUtil = static_cast<double>(overCapacity - 1);
    auto sLast = model.snapshotAt("GPU0", capacity - 1);
    ASSERT_TRUE(sLast.has_value());
    EXPECT_DOUBLE_EQ(sLast->utilizationPercent, expectedNewestUtil);

    // One past the last index is out of range.
    EXPECT_FALSE(model.snapshotAt("GPU0", capacity).has_value());
}

// ========== History window (#993) ==========

TEST(GPUModelTest, HistoryIsTrimmedToTheHistoryWindow)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.setMaxHistorySeconds(10.0);

    // One sample a second for 15 s: the cutoff is 15 - 10 = 5, so t = 5..15 remain, plus t = 4, the
    // newest before the cutoff, kept so the line runs off the window's left edge (#1016).
    // The cutoff falls exactly on t = 5, so start on a whole second: timestamps are seconds as double,
    // and from an arbitrary now() t = 5 could round to either side of the cutoff (11 or 12 samples).
    const auto start = std::chrono::ceil<std::chrono::seconds>(std::chrono::steady_clock::now());
    for (int i = 0; i <= 15; ++i)
    {
        rawProbe->withUtilization("GPU0", static_cast<double>(i));
        model.refreshAt(start + std::chrono::seconds(i));
    }

    const auto timestamps = model.historyTimestamps("GPU0");
    ASSERT_EQ(timestamps.size(), 12U);
    EXPECT_NEAR(timestamps.back() - timestamps.front(), 11.0, 1e-6);
    EXPECT_EQ(model.historyTimestamps().size(), 12U);

    const auto utilization = model.utilizationHistory("GPU0");
    ASSERT_EQ(utilization.size(), 12U);
    EXPECT_FLOAT_EQ(utilization.front(), 4.0F);
    EXPECT_FLOAT_EQ(utilization.back(), 15.0F);

    const auto publication = model.publication();
    const auto historyIt = publication->histories.find("GPU0");
    ASSERT_NE(historyIt, publication->histories.end());
    EXPECT_EQ(historyIt->second.timestamps.size(), 12U);
}

TEST(GPUModelTest, HistoryKeepsMoreThanThreeHundredSamplesWhenTheWindowAllows)
{
    // The old fixed 300-sample ring covered only 30 s of a 300 s window at a 100 ms refresh.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_DEFAULT);

    const auto start = std::chrono::steady_clock::now();
    constexpr int sampleCount = 1000; // 100 s at 100 ms
    for (int i = 0; i < sampleCount; ++i)
    {
        model.refreshAt(start + std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS * i));
    }

    EXPECT_EQ(model.historyTimestamps("GPU0").size(), static_cast<std::size_t>(sampleCount));
}

TEST(GPUModelTest, ShrinkingTheHistoryWindowTrimsExistingHistory)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.setMaxHistorySeconds(60.0);

    // A whole second, so the cutoff below lands exactly on t = 20 without rounding (see above).
    const auto start = std::chrono::ceil<std::chrono::seconds>(std::chrono::steady_clock::now());
    for (int i = 0; i <= 30; ++i)
    {
        model.refreshAt(start + std::chrono::seconds(i));
    }
    ASSERT_EQ(model.historyTimestamps("GPU0").size(), 31U);

    model.setMaxHistorySeconds(10.0);
    EXPECT_DOUBLE_EQ(model.maxHistorySeconds(), 10.0);
    // t = 20..30, plus t = 19 kept before the cutoff (#1016).
    EXPECT_EQ(model.historyTimestamps("GPU0").size(), 12U);
    EXPECT_EQ(model.historyTimestamps().size(), 12U);
}

TEST(GPUModelTest, MaxHistorySecondsIsClampedToTheSupportedRange)
{
    Domain::GPUModel model(std::make_unique<MockGPUProbe>());
    model.setMaxHistorySeconds(1.0);
    EXPECT_DOUBLE_EQ(model.maxHistorySeconds(), static_cast<double>(Domain::Sampling::HISTORY_SECONDS_MIN));
    model.setMaxHistorySeconds(1.0e9);
    EXPECT_DOUBLE_EQ(model.maxHistorySeconds(), static_cast<double>(Domain::Sampling::HISTORY_SECONDS_MAX));
}

TEST(GPUModelTest, AGpuAbsentForTheWholeWindowKeepsNoStaleSample)
{
    // Review of #1060: keeping a pre-cutoff anchor for a GPU with no sample inside the window would
    // join its last reading to the next one across the whole absence.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");
    rawProbe->withGPU("GPU1", "Other GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.setMaxHistorySeconds(10.0);

    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start);
    rawProbe->withoutGPUCounters("GPU1");
    for (int i = 1; i <= 20; ++i)
    {
        model.refreshAt(start + std::chrono::seconds(i));
    }

    EXPECT_TRUE(model.historyTimestamps("GPU1").empty());
    EXPECT_FALSE(model.historyTimestamps("GPU0").empty());
}

} // namespace
