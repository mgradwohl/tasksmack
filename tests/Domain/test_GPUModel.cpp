/// @file test_GPUModel.cpp
/// @brief Comprehensive tests for Domain::GPUModel
///
/// Tests cover:
/// - GPU enumeration and snapshot creation
/// - Memory utilization percentage calculations
/// - Multi-GPU scenarios
/// - Capability reporting
/// - Thread-safe operations

#include "Domain/GPUModel.h"
#include "Domain/SamplingConfig.h"
#include "Domain/SharedHistory.h"
#include "Mocks/MockGPUProbe.h"
#include "Platform/GPUTypes.h"
#include "PublicationLatency.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using TestMocks::makeGPUCounters;
using TestMocks::makeGPUInfo;
using TestMocks::MockGPUProbe;

namespace
{

/// The latest publication's history of @p gpuId: every series empty when the publication has none for it.
Domain::GPUPublishedHistory publishedHistory(const Domain::GPUModel& model, const std::string& gpuId)
{
    const auto publication = model.publication();
    const auto it = publication->histories.find(gpuId);
    return it != publication->histories.end() ? it->second : Domain::GPUPublishedHistory{};
}

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

TEST(GPUModelTest, NullProbeReportsPerProcessGpuDataKnownUnsupported)
{
    // The synthetic scenario (#1413) shares a GPUModel without a probe with ProcessesPanel: it must read
    // as known-unsupported, or the GPU columns never explain that they aren't supported.
    Domain::GPUModel model(nullptr);
    EXPECT_TRUE(model.perProcessMetricsKnownUnsupported());
    EXPECT_TRUE(model.perProcessUtilizationKnownUnsupported());
    const auto reading = model.readProcessGPUData();
    EXPECT_FALSE(reading.perProcessSupported);
    EXPECT_FALSE(reading.utilizationSupported);
    EXPECT_TRUE(reading.counters.empty());
    model.refresh();
    EXPECT_TRUE(model.perProcessMetricsKnownUnsupported());
    EXPECT_TRUE(model.perProcessUtilizationKnownUnsupported());
}

TEST(GPUModelTest, ProbeWithPerProcessSupportStillReportsSupported)
{
    // A real probe keeps reporting what its capabilities say.
    auto probe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    caps.hasPerProcessUtilization = true;
    probe->withCapabilities(caps);
    const Domain::GPUModel model(std::move(probe));
    EXPECT_FALSE(model.perProcessMetricsKnownUnsupported());
    EXPECT_FALSE(model.perProcessUtilizationKnownUnsupported());
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

TEST(GPUModelTest, PerProcessSupportFlagsFollowTheProbesCapabilities)
{
    // #1210: what the Processes table reads, once a frame, to tell which GPU columns it can fill.
    const auto modelWith = [](bool perProcess, bool utilization)
    {
        auto probe = std::make_unique<MockGPUProbe>();
        Platform::GPUCapabilities caps;
        caps.hasPerProcessMetrics = perProcess;
        caps.hasPerProcessUtilization = utilization;
        probe->withCapabilities(caps);
        return std::make_unique<Domain::GPUModel>(std::move(probe));
    };

    const auto pdhLike = modelWith(true, true);
    EXPECT_FALSE(pdhLike->perProcessMetricsKnownUnsupported());
    EXPECT_FALSE(pdhLike->perProcessUtilizationKnownUnsupported());

    // NVML: per-process memory and engines, no utilization.
    const auto nvmlLike = modelWith(true, false);
    EXPECT_FALSE(nvmlLike->perProcessMetricsKnownUnsupported());
    EXPECT_TRUE(nvmlLike->perProcessUtilizationKnownUnsupported());

    // DRM / ROCm: nothing per process.
    const auto drmLike = modelWith(false, false);
    EXPECT_TRUE(drmLike->perProcessMetricsKnownUnsupported());
    EXPECT_TRUE(drmLike->perProcessUtilizationKnownUnsupported());
}

TEST(GPUModelTest, ProcessGpuDataComesWithTheSupportItWasReadUnder)
{
    // #1210: the counters and the flags from one operation, so a generation is never stamped
    // supported while the read short-circuited empty, or the reverse.
    auto probe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    caps.hasPerProcessUtilization = false; // NVML-like
    probe->withCapabilities(caps);
    probe->withProcessGPU(100, "GPU0", 1024ULL * 1024);
    auto* rawProbe = probe.get();
    Domain::GPUModel model(std::move(probe));

    const auto reading = model.readProcessGPUData();
    EXPECT_TRUE(reading.perProcessSupported);
    EXPECT_FALSE(reading.utilizationSupported);
    EXPECT_EQ(reading.counters.size(), 1U);

    // Lost on re-enumeration: no read, and the reading says so.
    Platform::GPUCapabilities none;
    rawProbe->withCapabilities(none).withRescanReportingChange();
    model.refresh();
    const auto after = model.readProcessGPUData();
    EXPECT_FALSE(after.perProcessSupported);
    EXPECT_FALSE(after.utilizationSupported);
    EXPECT_TRUE(after.counters.empty());
}

TEST(GPUModelTest, AFailedProcessGpuReadStillReportsTheSupportItRanUnder)
{
    // #1210: a throwing read returned no flags, so the caller fell back to flags it had loaded
    // outside the probe lock, which a rescan could have changed in between. The failure now comes
    // back with the flags taken under the lock.
    auto probe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    caps.hasPerProcessUtilization = true;
    probe->withCapabilities(caps);
    probe->withProcessGPU(100, "GPU0", 1024ULL * 1024).withProcessCountersThrowing();
    Domain::GPUModel model(std::move(probe));

    Domain::GPUModel::ProcessGPUReading reading;
    ASSERT_NO_THROW(reading = model.readProcessGPUData());
    EXPECT_TRUE(reading.perProcessSupported);
    EXPECT_TRUE(reading.utilizationSupported);
    EXPECT_TRUE(reading.counters.empty());
    EXPECT_TRUE(reading.failure != nullptr);

    // The counters-only call still throws, as before.
    EXPECT_ANY_THROW(static_cast<void>(model.readProcessGPUCounters()));
}

// =============================================================================
// Per-process GPU publication (#1417): the GPU sampler is the single owner of GPU acquisition
// =============================================================================

namespace
{

/// A probe with GPU0 ("Test GPU", also known by a LUID id) and pid 100 using it, per-process supported.
std::unique_ptr<MockGPUProbe> makePerProcessProbe()
{
    auto probe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    caps.hasPerProcessUtilization = true;
    probe->withCapabilities(caps);
    probe->withSharedMemoryGPU("GPU0", "Test GPU", "TestVendor").withProcessGPU(100, "GPU0", 1024ULL * 1024);
    return probe;
}

} // namespace

TEST(GPUModelTest, NothingIsPublishedPerProcessBeforeTheFirstRefresh)
{
    const Domain::GPUModel model(makePerProcessProbe());
    const auto publication = model.processGPUPublication();
    ASSERT_NE(publication, nullptr);
    EXPECT_EQ(publication->version, 0U);
    EXPECT_EQ(model.processGPUPublicationVersion(), 0U);
    EXPECT_TRUE(publication->counters.empty());
}

TEST(GPUModelTest, RefreshPublishesPerProcessCountersWithTheirCaptureTime)
{
    const auto start = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    auto probe = makePerProcessProbe();
    auto* rawProbe = probe.get();
    Domain::GPUModel model(std::move(probe));

    model.refreshAt(start);
    auto publication = model.processGPUPublication();
    EXPECT_EQ(publication->version, 1U);
    EXPECT_EQ(model.processGPUPublicationVersion(), 1U);
    EXPECT_EQ(publication->captureTime, start);
    EXPECT_TRUE(publication->perProcessSupported);
    EXPECT_TRUE(publication->utilizationSupported);
    EXPECT_FALSE(publication->readFailed);
    ASSERT_EQ(publication->counters.size(), 1U);
    EXPECT_EQ(publication->counters[0].pid, 100);
    ASSERT_EQ(publication->adapters.size(), 1U);
    EXPECT_EQ(publication->adapters[0].id, "GPU0");
    EXPECT_EQ(publication->adapters[0].name, "Test GPU");
    EXPECT_TRUE(publication->adapters[0].isIntegrated);
    EXPECT_TRUE(publication->adapters[0].memoryIsShared);
    EXPECT_EQ(rawProbe->readProcessCountersCallCount(), 1U) << "one per-process read per refresh";

    // Each refresh publishes a newer generation; one already handed out stays as it was.
    rawProbe->withProcessGPU(200, "GPU0", 2048);
    model.refreshAt(start + std::chrono::seconds(1));
    const auto next = model.processGPUPublication();
    EXPECT_EQ(next->version, 2U);
    EXPECT_EQ(next->captureTime, start + std::chrono::seconds(1));
    EXPECT_EQ(next->counters.size(), 2U);
    EXPECT_EQ(publication->counters.size(), 1U);
    EXPECT_EQ(rawProbe->readProcessCountersCallCount(), 2U);
}

TEST(GPUModelTest, UnsupportedPerProcessDataIsPublishedWithoutReadingTheProbe)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Intel DRM").withProcessGPU(100, "GPU0", 1024); // Capabilities: none per process
    auto* rawProbe = probe.get();
    Domain::GPUModel model(std::move(probe));

    model.refresh();
    const auto publication = model.processGPUPublication();
    EXPECT_EQ(publication->version, 1U);
    EXPECT_FALSE(publication->perProcessSupported);
    EXPECT_FALSE(publication->readFailed);
    EXPECT_TRUE(publication->counters.empty());
    EXPECT_EQ(rawProbe->readProcessCountersCallCount(), 0U);
}

TEST(GPUModelTest, AFailingPerProcessReadIsPublishedAsAFailedReadAndTheSystemStillUpdates)
{
    auto probe = makePerProcessProbe();
    probe->withProcessCountersThrowing();
    Domain::GPUModel model(std::move(probe));

    ASSERT_NO_THROW(model.refresh());
    const auto publication = model.processGPUPublication();
    EXPECT_EQ(publication->version, 1U);
    EXPECT_TRUE(publication->perProcessSupported);
    EXPECT_TRUE(publication->readFailed);
    EXPECT_TRUE(publication->counters.empty());
    EXPECT_EQ(model.publicationVersion(), 1U) << "the system GPU data is published regardless";
    EXPECT_EQ(model.snapshots().size(), 1U);
}

TEST(GPUModelTest, AFailingSystemReadStillPublishesThePerProcessCounters)
{
    auto probe = makePerProcessProbe();
    probe->withCountersThrowing();
    Domain::GPUModel model(std::move(probe));

    ASSERT_NO_THROW(model.refresh());
    EXPECT_EQ(model.publicationVersion(), 0U) << "no system reading, nothing published for it";
    const auto publication = model.processGPUPublication();
    EXPECT_EQ(publication->version, 1U);
    EXPECT_FALSE(publication->readFailed);
    EXPECT_EQ(publication->counters.size(), 1U);
}

TEST(GPUModelTest, NoProbePublishesNothingPerProcess)
{
    Domain::GPUModel model(nullptr);
    model.refresh();
    EXPECT_EQ(model.processGPUPublicationVersion(), 0U);
    EXPECT_TRUE(model.perProcessMetricsKnownUnsupported());
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

// #1117: a GPU the probe left asleep is carried through to the snapshot, so the UI can say so, and
// its unread readings publish as gaps.
TEST(GPUModelTest, SuspendedGpuIsMarkedInTheSnapshot)
{
    auto counters = makeGPUCounters("GPU0");
    counters.suspended = true;
    counters.utilizationAvailable = false;
    counters.memoryAvailable = false;
    counters.memoryTotalBytes = 8ULL * 1024 * 1024 * 1024;
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Sleepy GPU", "NVIDIA").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_TRUE(snaps[0].suspended);
    EXPECT_EQ(snaps[0].memoryTotalBytes, 8ULL * 1024 * 1024 * 1024);
    const auto utilization = publishedHistory(model, "GPU0").utilization;
    ASSERT_EQ(utilization.size(), 1U);
    EXPECT_TRUE(std::isnan(utilization[0]));
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
    // Left unclamped, matching memoryUsedPercent: a raw reading above the
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
    // An unavailable sample must come back as NaN, not 0.0F, from the published history (what
    // GpuSection's chart/tooltip actually consume) - otherwise a caller can't tell "fan genuinely
    // at 0%" from "couldn't read the fan this poll".
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

    // Sanity check the available-sample sibling model is NOT NaN, so this test would actually
    // fail if fanSpeedAvailable stopped being honored.
    const auto availableFanHistory = publishedHistory(model, "GPU0").fanSpeed;
    ASSERT_EQ(availableFanHistory.size(), 1);
    EXPECT_FALSE(std::isnan(availableFanHistory[0]));
    EXPECT_FLOAT_EQ(availableFanHistory[0], 66.0F);
}

TEST(GPUModelTest, PowerIsDerivedFromTheEnergyCounter)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    auto counters = makeGPUCounters("GPU0");
    counters.powerAvailable = false;
    counters.energyAvailable = true;
    counters.energyMicroJoules = 1'000'000'000;
    rawProbe->withGPU("GPU0", "Test GPU", "Intel").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start);
    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_FALSE(snaps[0].powerAvailable); // No previous counter yet: a gap, not 0 W

    counters.energyMicroJoules += 30'000'000; // +30 J over 2 s
    rawProbe->withGPUCounters("GPU0", counters);
    model.refreshAt(start + std::chrono::seconds(2));
    snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_TRUE(snaps[0].powerAvailable);
    EXPECT_DOUBLE_EQ(snaps[0].powerDrawWatts, 15.0);
}

TEST(GPUModelTest, EnergyCounterGapOrResetLeavesPowerUnread)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    auto counters = makeGPUCounters("GPU0");
    counters.powerAvailable = false;
    counters.energyAvailable = true;
    counters.energyMicroJoules = 5'000'000;
    rawProbe->withGPU("GPU0", "Test GPU", "Intel").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start);

    // A failed read (or a suspended card) has no counter...
    counters.energyAvailable = false;
    rawProbe->withGPUCounters("GPU0", counters);
    model.refreshAt(start + std::chrono::seconds(1));
    EXPECT_FALSE(model.snapshots()[0].powerAvailable);

    // ...so the next sample has nothing to take a delta against.
    counters.energyAvailable = true;
    counters.energyMicroJoules = 9'000'000;
    rawProbe->withGPUCounters("GPU0", counters);
    model.refreshAt(start + std::chrono::seconds(2));
    EXPECT_FALSE(model.snapshots()[0].powerAvailable);

    // A counter that went backwards (driver reload) is not a huge or negative draw.
    counters.energyMicroJoules = 10;
    rawProbe->withGPUCounters("GPU0", counters);
    model.refreshAt(start + std::chrono::seconds(3));
    EXPECT_FALSE(model.snapshots()[0].powerAvailable);

    counters.energyMicroJoules = 4'000'010; // +4 J over 1 s
    rawProbe->withGPUCounters("GPU0", counters);
    model.refreshAt(start + std::chrono::seconds(4));
    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_TRUE(snaps[0].powerAvailable);
    EXPECT_DOUBLE_EQ(snaps[0].powerDrawWatts, 4.0);
}

// A re-enumeration (#1116) that keeps a GPU keeps its previous energy reading, so the power draw
// carries on across it rather than going unread for a sample.
TEST(GPUModelTest, PowerFromTheEnergyCounterCarriesOnAcrossAReEnumeration)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    auto counters = makeGPUCounters("GPU0");
    counters.powerAvailable = false;
    counters.energyAvailable = true;
    counters.energyMicroJoules = 1'000'000;
    rawProbe->withGPU("GPU0", "Test GPU", "Intel").withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start);

    rawProbe->withGPU("eGPU", "Hot-plugged GPU", "Intel").withRescanReportingChange();
    counters.energyMicroJoules += 6'000'000; // +6 J over 2 s
    rawProbe->withGPUCounters("GPU0", counters);
    model.refreshAt(start + std::chrono::seconds(2));

    ASSERT_EQ(model.gpuInfo().size(), 2U); // The rescan was taken up
    const auto snaps = model.snapshots();
    const auto gpu0 = std::ranges::find(snaps, std::string("GPU0"), &Domain::GPUSnapshot::gpuId);
    ASSERT_NE(gpu0, snaps.end());
    EXPECT_TRUE(gpu0->powerAvailable);
    EXPECT_DOUBLE_EQ(gpu0->powerDrawWatts, 3.0);
}

// =============================================================================
// Utilization from DRM clients' engine busyness (#1267)
// =============================================================================

constexpr auto RENDER_CLASS = static_cast<std::size_t>(Platform::GPUEngineClass::Render);
constexpr auto VIDEO_CLASS = static_cast<std::size_t>(Platform::GPUEngineClass::Video);

/// A DRM client with one engine class's cumulative busy/total counters.
[[nodiscard]] Platform::GPUEngineClientCounters
engineClient(std::uint64_t clientId, std::size_t engineClass, std::uint64_t busy, std::uint64_t total, std::uint32_t capacity = 1)
{
    Platform::GPUEngineClientCounters client;
    client.clientId = clientId;
    client.engines.at(engineClass) = Platform::GPUEngineBusyCounter{.available = true, .busy = busy, .total = total, .capacity = capacity};
    return client;
}

/// GPU0's counters as the DRM probe reports them: engine busyness, but no utilization of its own.
[[nodiscard]] Platform::GPUCounters engineCounters(std::vector<Platform::GPUEngineClientCounters> clients)
{
    auto counters = makeGPUCounters("GPU0");
    counters.utilizationAvailable = false;
    counters.utilizationPercent = 0.0;
    counters.engineBusyAvailable = true;
    counters.engineClients = std::move(clients);
    return counters;
}

// Per class, the clients' busy shares are summed over the class's engine count; the busiest class wins.
TEST(GPUModelTest, UtilizationIsTheBusiestEngineClassAcrossClients)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "Intel")
        .withGPUCounters("GPU0",
                         engineCounters({engineClient(1, RENDER_CLASS, 0, 1000),
                                         engineClient(2, RENDER_CLASS, 0, 1000),
                                         engineClient(3, VIDEO_CLASS, 0, 1000, 2)}));
    Domain::GPUModel model(std::move(probe));
    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start);
    EXPECT_FALSE(model.snapshots()[0].utilizationAvailable); // No previous sample: a gap, not 0%

    // Render: 100 + 200 busy of 1000 = 30%. Video: 900 of 1000 on one of its two engines = 45%.
    rawProbe->withGPUCounters("GPU0",
                              engineCounters({engineClient(1, RENDER_CLASS, 100, 2000),
                                              engineClient(2, RENDER_CLASS, 200, 2000),
                                              engineClient(3, VIDEO_CLASS, 900, 2000, 2)}));
    model.refreshAt(start + std::chrono::seconds(1));
    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_TRUE(snaps[0].utilizationAvailable);
    EXPECT_DOUBLE_EQ(snaps[0].utilizationPercent, 45.0);
}

TEST(GPUModelTest, EngineUtilizationIgnoresClientsSeenOnceAndCountersThatWentBack)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "Intel")
        .withGPUCounters("GPU0",
                         engineCounters({engineClient(1, RENDER_CLASS, 500, 1000),
                                         engineClient(2, RENDER_CLASS, 900, 1000),
                                         engineClient(4, RENDER_CLASS, 0, 1000)}));
    Domain::GPUModel model(std::move(probe));
    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start);

    // Client 1 exited, client 3 is new (its lifetime busyness isn't this interval's), client 2's
    // counter went backwards; only client 4, busy 100 of 1000, counts.
    rawProbe->withGPUCounters("GPU0",
                              engineCounters({engineClient(2, RENDER_CLASS, 10, 2000),
                                              engineClient(3, RENDER_CLASS, 5'000, 2000),
                                              engineClient(4, RENDER_CLASS, 100, 2000)}));
    model.refreshAt(start + std::chrono::seconds(1));
    EXPECT_TRUE(model.snapshots()[0].utilizationAvailable);
    EXPECT_DOUBLE_EQ(model.snapshots()[0].utilizationPercent, 10.0);

    // No clients at all: the GPU is idle.
    rawProbe->withGPUCounters("GPU0", engineCounters({}));
    model.refreshAt(start + std::chrono::seconds(2));
    EXPECT_TRUE(model.snapshots()[0].utilizationAvailable);
    EXPECT_DOUBLE_EQ(model.snapshots()[0].utilizationPercent, 0.0);

    // A sample without busyness (a suspended card) leaves the next one nothing to compare against.
    auto suspended = engineCounters({});
    suspended.engineBusyAvailable = false;
    rawProbe->withGPUCounters("GPU0", suspended);
    model.refreshAt(start + std::chrono::seconds(3));
    EXPECT_FALSE(model.snapshots()[0].utilizationAvailable);
    rawProbe->withGPUCounters("GPU0", engineCounters({engineClient(4, RENDER_CLASS, 200, 3000)}));
    model.refreshAt(start + std::chrono::seconds(4));
    EXPECT_FALSE(model.snapshots()[0].utilizationAvailable);
}

TEST(GPUModelTest, EngineUtilizationKeepsABusyCounterAtItsHighWaterMarkUntilItCatchesUp)
{
    // #1350 review: the DRM usage-stats contract lets a busy counter dip briefly and asks userspace
    // to keep the larger value until it catches up. The dip itself counts nothing, and a rise that is
    // still below the old value must count nothing either -- only busyness past it is new.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "Intel").withGPUCounters("GPU0", engineCounters({engineClient(7, RENDER_CLASS, 800, 1000)}));
    Domain::GPUModel model(std::move(probe));
    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start);

    rawProbe->withGPUCounters("GPU0", engineCounters({engineClient(7, RENDER_CLASS, 600, 2000)})); // dipped
    model.refreshAt(start + std::chrono::seconds(1));
    EXPECT_DOUBLE_EQ(model.snapshots()[0].utilizationPercent, 0.0);

    rawProbe->withGPUCounters("GPU0", engineCounters({engineClient(7, RENDER_CLASS, 750, 3000)})); // still below 800
    model.refreshAt(start + std::chrono::seconds(2));
    EXPECT_DOUBLE_EQ(model.snapshots()[0].utilizationPercent, 0.0) << "a rise below the high-water mark is not new busyness";

    rawProbe->withGPUCounters("GPU0", engineCounters({engineClient(7, RENDER_CLASS, 1'000, 4000)})); // 200 past 800
    model.refreshAt(start + std::chrono::seconds(3));
    EXPECT_DOUBLE_EQ(model.snapshots()[0].utilizationPercent, 20.0);
}

TEST(GPUModelTest, EngineUtilizationIsCappedAtOneHundredPercent)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "Intel")
        .withGPUCounters("GPU0", engineCounters({engineClient(1, RENDER_CLASS, 0, 1000), engineClient(2, RENDER_CLASS, 0, 1000)}));
    Domain::GPUModel model(std::move(probe));
    const auto start = std::chrono::steady_clock::now();
    model.refreshAt(start);

    // Reads moments apart: each client's share is measured over a slightly different interval.
    rawProbe->withGPUCounters("GPU0", engineCounters({engineClient(1, RENDER_CLASS, 600, 2000), engineClient(2, RENDER_CLASS, 500, 2000)}));
    model.refreshAt(start + std::chrono::seconds(1));
    EXPECT_DOUBLE_EQ(model.snapshots()[0].utilizationPercent, 100.0);
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
    auto hist0 = publishedHistory(model, "GPU0").utilization;
    auto hist1 = publishedHistory(model, "GPU1").utilization;

    EXPECT_EQ(hist0.size(), 2);
    EXPECT_EQ(hist1.size(), 2);

    // Verify latest values (last element in vector)
    EXPECT_FLOAT_EQ(hist0.back(), 60.0F);
    EXPECT_FLOAT_EQ(hist1.back(), 80.0F);
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

TEST(GPUModelTest, HistoryForNonexistentGPUReturnsEmpty)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    EXPECT_TRUE(publishedHistory(model, "NonexistentGPU").timestamps.empty());
    EXPECT_FALSE(model.publication()->histories.contains("NonexistentGPU"));
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

    auto hist = publishedHistory(model, "GPU0").utilization;
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

    auto hist = publishedHistory(model, "NonexistentGPU").utilization;
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

    auto hist = publishedHistory(model, "GPU0").memoryPercent;
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

    auto hist = publishedHistory(model, "NonexistentGPU").memoryPercent;
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

    auto hist = publishedHistory(model, "GPU0").gpuClock;
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

    auto hist = publishedHistory(model, "NonexistentGPU").gpuClock;
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

    auto hist = publishedHistory(model, "GPU0").encoder;
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

    auto hist = publishedHistory(model, "NonexistentGPU").encoder;
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

    auto hist = publishedHistory(model, "GPU0").decoder;
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

    auto hist = publishedHistory(model, "NonexistentGPU").decoder;
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

    auto hist = publishedHistory(model, "GPU0").temperature;
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

    auto hist = publishedHistory(model, "NonexistentGPU").temperature;
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

    auto hist = publishedHistory(model, "GPU0").power;
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

    auto hist = publishedHistory(model, "NonexistentGPU").power;
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

    auto hist = publishedHistory(model, "GPU0").fanSpeed;
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

    auto hist = publishedHistory(model, "NonexistentGPU").fanSpeed;
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

    auto timestamps = publishedHistory(model, "GPU0").timestamps;
    auto utilHist = publishedHistory(model, "GPU0").utilization;

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

    auto timestamps = publishedHistory(model, "GPU0").timestamps;
    EXPECT_TRUE(timestamps.empty());
}

// =============================================================================
// Per-GPU history timestamps Tests
// =============================================================================

TEST(GPUModelTest, PerGpuHistoryTimestampsEmptyForUnknownGpu)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    EXPECT_TRUE(publishedHistory(model, "GPU_UNKNOWN").timestamps.empty());
}

TEST(GPUModelTest, PerGpuHistoryTimestampsEmptyWhenNoRefresh)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    // No refresh called

    EXPECT_TRUE(publishedHistory(model, "GPU0").timestamps.empty());
}

TEST(GPUModelTest, PerGpuHistoryTimestampsLengthMatchesHistory)
{
    // A GPU's published timestamps must have the same number of entries
    // as its utilization history so indices stay aligned.
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 10.0);

    Domain::GPUModel model(std::move(probe));
    for (int i = 0; i < 4; ++i)
    {
        rawProbe->withUtilization("GPU0", static_cast<double>(i) * 10.0);
        model.refresh();
    }

    const auto timestamps = publishedHistory(model, "GPU0").timestamps;
    const auto utilHist = publishedHistory(model, "GPU0").utilization;
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

    const auto timestamps = publishedHistory(model, "GPU0").timestamps;
    ASSERT_GE(timestamps.size(), 2u);
    for (std::size_t i = 1; i < timestamps.size(); ++i)
    {
        EXPECT_GT(timestamps[i], timestamps[i - 1]);
    }
}

TEST(GPUModelTest, PerGpuHistoryTimestampsIndexAlignedWithThePublishedHistory)
{
    // Each published series has one entry per published timestamp. This is the alignment GpuSection relies on for
    // tooltip timestamps.
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

    const auto timestamps = publishedHistory(model, "GPU0").timestamps;
    ASSERT_EQ(static_cast<int>(timestamps.size()), kSamples);
    const auto& published = model.publication()->histories.at("GPU0");
    ASSERT_EQ(published.timestamps.size(), timestamps.size());
    ASSERT_EQ(published.utilization.size(), timestamps.size());
    for (std::size_t i = 0; i < timestamps.size(); ++i)
    {
        EXPECT_DOUBLE_EQ(timestamps[i], published.timestamps[i]);
        EXPECT_FLOAT_EQ(published.utilization[i], static_cast<float>(i) * 10.0F);
    }
}

TEST(GPUModelTest, PerGpuHistoryTimestampsCappedAtCapacity)
{
    // After pushing more samples than the ring holds (all at once, so the time window
    // trims nothing), the published timestamps hold exactly the capacity.
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

    EXPECT_EQ(publishedHistory(model, "GPU0").timestamps.size(), capacity);
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

    const auto ts0 = publishedHistory(model, "GPU0").timestamps;
    const auto ts1 = publishedHistory(model, "GPU1").timestamps;
    EXPECT_EQ(ts0.size(), 3u);
    EXPECT_EQ(ts1.size(), 3u);

    // Each set of timestamps must match its own history length.
    EXPECT_EQ(ts0.size(), publishedHistory(model, "GPU0").utilization.size());
    EXPECT_EQ(ts1.size(), publishedHistory(model, "GPU1").utilization.size());
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
    good.memoryUsedBytes = 2ULL * 1024 * 1024 * 1024;
    good.memoryTotalBytes = 8ULL * 1024 * 1024 * 1024;
    rawProbe->withGPUCounters("GPU0", good);

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    Platform::GPUCounters failed = good;
    failed.utilizationAvailable = false;
    failed.temperatureAvailable = false;
    failed.powerAvailable = false;
    failed.gpuClockAvailable = false;
    failed.memoryAvailable = false;
    failed.memoryUsedBytes = 0; // Windows keeps DXGI's total when NVML's memory read fails

    failed.utilizationPercent = 0.0;
    failed.temperatureC = 0;
    failed.powerDrawWatts = 0.0;
    failed.gpuClockMHz = 0;
    rawProbe->withGPUCounters("GPU0", failed);
    model.refresh();

    for (const auto& series : {publishedHistory(model, "GPU0").utilization,
                               publishedHistory(model, "GPU0").temperature,
                               publishedHistory(model, "GPU0").power,
                               publishedHistory(model, "GPU0").gpuClock,
                               publishedHistory(model, "GPU0").memoryPercent})
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
    EXPECT_TRUE(std::isnan(published.memoryPercent.back()));
    EXPECT_FLOAT_EQ(published.memoryPercent.front(), 25.0F);
    // No placeholder byte figures for the unread sample: a 0 total means the tooltip shows N/A.
    EXPECT_EQ(published.memoryUsedBytes.back(), 0U);
    EXPECT_EQ(published.memoryTotalBytes.back(), 0U);
    EXPECT_EQ(published.memoryTotalBytes.front(), 8ULL * 1024 * 1024 * 1024);
    EXPECT_FLOAT_EQ(published.utilization.front(), 40.0F);

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_FALSE(snaps[0].utilizationAvailable);
    EXPECT_FALSE(snaps[0].temperatureAvailable);
    EXPECT_FALSE(snaps[0].memoryAvailable);
}

TEST(GPUModelTest, ZeroGpuClockIsAGapLikeItsNowBar)
{
    // A 0 MHz clock is the probes' "couldn't read it" and the Clock NowBar shows N/A for it (#995);
    // the history has a gap there too, so the line and the bar agree (#1111).
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");
    Platform::GPUCounters counters;
    counters.gpuId = "GPU0";
    counters.gpuClockMHz = 1500;
    rawProbe->withGPUCounters("GPU0", counters);

    Domain::GPUModel model(std::move(probe));
    model.refresh();
    counters.gpuClockMHz = 0;
    rawProbe->withGPUCounters("GPU0", counters);
    model.refresh();

    const auto hist = publishedHistory(model, "GPU0").gpuClock;
    ASSERT_EQ(hist.size(), 2U);
    EXPECT_FLOAT_EQ(hist[0], 1500.0F);
    EXPECT_TRUE(std::isnan(hist[1]));
    const auto publication = model.publication();
    EXPECT_TRUE(std::isnan(publication->histories.at("GPU0").gpuClock.back()));
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

    // One timestamp per refresh, the one GPU0 was missing from included.
    const auto perGpuTs = publishedHistory(model, "GPU0").timestamps;
    ASSERT_EQ(perGpuTs.size(), 4U);
    for (std::size_t i = 1; i < perGpuTs.size(); ++i)
    {
        EXPECT_GT(perGpuTs[i], perGpuTs[i - 1]);
    }

    const auto utilHist = publishedHistory(model, "GPU0").utilization;
    ASSERT_EQ(utilHist.size(), 4U);
    EXPECT_FLOAT_EQ(utilHist[1], 30.0F);
    EXPECT_TRUE(std::isnan(utilHist[2]));
    EXPECT_FLOAT_EQ(utilHist[3], 50.0F);
    EXPECT_TRUE(std::isnan(publishedHistory(model, "GPU0").temperature[2]));

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
    EXPECT_EQ(publishedHistory(model, "GPU0").timestamps.size(), 6U); // Still in the window: one sample, five gaps

    for (int i = 6; i <= 30; ++i)
    {
        model.refreshAt(start + std::chrono::seconds(i));
    }
    EXPECT_TRUE(publishedHistory(model, "GPU0").timestamps.empty());
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

    auto hist0 = publishedHistory(model, "GPU0").utilization;
    auto hist1 = publishedHistory(model, "GPU1").utilization;

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

    // Reader thread that reads the published history while the model refreshes. We verify it
    // doesn't crash and every generation it sees is whole: each series as long as its timestamps.
    auto reader = std::jthread(
        [&](std::stop_token st)
        {
            while (!stop.load() && !st.stop_requested())
            {
                const auto history = publishedHistory(model, "GPU0");
                const std::size_t samples = history.timestamps.size();

                // Verify we got non-empty, aligned results (at least one refresh happened)
                if (samples == 0 || history.utilization.size() != samples || history.memoryPercent.size() != samples ||
                    history.gpuClock.size() != samples || history.encoder.size() != samples || history.decoder.size() != samples ||
                    history.temperature.size() != samples || history.power.size() != samples || history.fanSpeed.size() != samples)
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
// History by index
// =============================================================================

TEST(GPUModelTest, HistoryOfAnUnknownGpuIsEmpty)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    EXPECT_TRUE(publishedHistory(model, "GPU_UNKNOWN").utilization.empty());
    EXPECT_TRUE(publishedHistory(model, "GPU_UNKNOWN").timestamps.empty());
}

TEST(GPUModelTest, HistoryIsEmptyBeforeAnyRefresh)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    // No refresh called

    EXPECT_TRUE(publishedHistory(model, "GPU0").utilization.empty());
    EXPECT_TRUE(publishedHistory(model, "GPU0").timestamps.empty());
    EXPECT_EQ(model.publicationVersion(), 0U);
}

TEST(GPUModelTest, HistoryHoldsEachSampleInOrder)
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

    const auto utilization = publishedHistory(model, "GPU0").utilization;
    ASSERT_EQ(utilization.size(), 3U);
    EXPECT_FLOAT_EQ(utilization[0], 10.0F);
    EXPECT_FLOAT_EQ(utilization[1], 20.0F);
    EXPECT_FLOAT_EQ(utilization[2], 30.0F);
}

TEST(GPUModelTest, HistoryKeepsTheNewestSamplesOnceItsCapacityIsExceeded)
{
    // Push more samples than the series' capacity: the oldest retained sample comes first and the
    // newest last.
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
    const auto utilization = publishedHistory(model, "GPU0").utilization;
    ASSERT_EQ(utilization.size(), capacity);
    EXPECT_EQ(publishedHistory(model, "GPU0").timestamps.size(), capacity);
    EXPECT_FLOAT_EQ(utilization.front(), static_cast<float>(overCapacity - capacity));
    EXPECT_FLOAT_EQ(utilization.back(), static_cast<float>(overCapacity - 1));
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

    const auto timestamps = publishedHistory(model, "GPU0").timestamps;
    ASSERT_EQ(timestamps.size(), 12U);
    EXPECT_NEAR(timestamps.back() - timestamps.front(), 11.0, 1e-6);

    const auto utilization = publishedHistory(model, "GPU0").utilization;
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

    EXPECT_EQ(publishedHistory(model, "GPU0").timestamps.size(), static_cast<std::size_t>(sampleCount));
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
    ASSERT_EQ(publishedHistory(model, "GPU0").timestamps.size(), 31U);

    model.setMaxHistorySeconds(10.0);
    EXPECT_DOUBLE_EQ(model.maxHistorySeconds(), 10.0);
    // t = 20..30, plus t = 19 kept before the cutoff (#1016).
    EXPECT_EQ(publishedHistory(model, "GPU0").timestamps.size(), 12U);
}

// #1145: the trimmed history is published at once, not at the next sample, which can be seconds away.
TEST(GPUModelTest, ShrinkingTheHistoryWindowRepublishesTheTrimmedHistory)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.setMaxHistorySeconds(60.0);

    const auto start = std::chrono::ceil<std::chrono::seconds>(std::chrono::steady_clock::now());
    for (int i = 0; i <= 30; ++i)
    {
        model.refreshAt(start + std::chrono::seconds(i));
    }
    const std::uint64_t versionBefore = model.publicationVersion();
    ASSERT_EQ(model.publication()->histories.at("GPU0").timestamps.size(), 31U);

    model.setMaxHistorySeconds(10.0);

    EXPECT_GT(model.publicationVersion(), versionBefore);
    const auto publication = model.publication();
    EXPECT_EQ(publication->version, model.publicationVersion());
    // t = 20..30, plus t = 19 kept before the cutoff (#1016).
    EXPECT_EQ(publication->histories.at("GPU0").timestamps.size(), 12U);
    EXPECT_EQ(publication->histories.at("GPU0").utilization.size(), 12U);
}

TEST(GPUModelTest, ChangingTheHistoryWindowBeforeAnyRefreshPublishesNothing)
{
    Domain::GPUModel model(std::make_unique<MockGPUProbe>());
    model.setMaxHistorySeconds(60.0);
    EXPECT_EQ(model.publicationVersion(), 0U);
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

    EXPECT_TRUE(publishedHistory(model, "GPU1").timestamps.empty());
    EXPECT_FALSE(publishedHistory(model, "GPU0").timestamps.empty());
}

// =============================================================================
// Re-enumeration (#1116, #1289)
// =============================================================================

// Without a reported change the GPU list is not re-read: every refresh asks the
// probe for a quick rescan, and nothing more.
TEST(GPUModelTest, NoReenumerationWhileTheProbeReportsNoChange)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    for (int i = 0; i < 3; ++i)
    {
        model.refresh();
    }

    EXPECT_EQ(rawProbe->enumerateCallCount(), 1U); // the constructor's
    EXPECT_EQ(rawProbe->quickRescanCount(), 3U);
    EXPECT_EQ(rawProbe->fullRescanCount(), 0U);
}

// A full rescan, which may look for hot-plugged or lost devices, is asked for
// once per GPU_RESCAN_INTERVAL_SECONDS; the refreshes in between get a quick
// one.
TEST(GPUModelTest, FullRescanIsAskedForOncePerInterval)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    const auto start = std::chrono::ceil<std::chrono::seconds>(std::chrono::steady_clock::now());
    const int interval = Domain::Sampling::GPU_RESCAN_INTERVAL_SECONDS;
    for (int second = 1; second <= interval * 2; ++second)
    {
        model.refreshAt(start + std::chrono::seconds(second));
    }

    // Construction counts as a full rescan, so the first comes one interval after
    // it, the second one interval after that.
    EXPECT_EQ(rawProbe->fullRescanCount(), 2U);
    EXPECT_EQ(rawProbe->quickRescanCount(), static_cast<std::uint32_t>((interval * 2) - 2));
    EXPECT_EQ(rawProbe->enumerateCallCount(), 1U);
}

// #1116: when the probe reports a change, the GPU list is re-read and
// published. A GPU that persists keeps its history; a new one is added with a
// history of its own.
TEST(GPUModelTest, ReEnumeratesAndPublishesAGpuAddedAfterConstruction)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withUtilization("GPU0", 10.0);

    Domain::GPUModel model(std::move(probe));
    model.refresh();
    ASSERT_EQ(model.publication()->gpuInfo.size(), 1U);

    rawProbe->withGPU("eGPU", "Hot-plugged GPU", "TestVendor").withUtilization("eGPU", 40.0);
    rawProbe->withRescanReportingChange();
    model.refresh();

    EXPECT_EQ(rawProbe->enumerateCallCount(), 2U);
    const auto publication = model.publication();
    ASSERT_EQ(publication->gpuInfo.size(), 2U);
    EXPECT_EQ(publication->gpuInfo[1].id, "eGPU");
    EXPECT_EQ(publication->gpuInfo[1].name, "Hot-plugged GPU");
    EXPECT_EQ(model.gpuInfo().size(), 2U);
    EXPECT_EQ(publishedHistory(model, "GPU0").utilization.size(),
              2U); // carried on across the re-enumeration
    ASSERT_EQ(publishedHistory(model, "eGPU").utilization.size(), 1U);
    EXPECT_FLOAT_EQ(publishedHistory(model, "eGPU").utilization[0], 40.0F);

    // The snapshot takes its identity from the new GPU info.
    const auto snaps = model.snapshots();
    const auto eGpu = std::ranges::find(snaps, std::string("eGPU"), &Domain::GPUSnapshot::gpuId);
    ASSERT_NE(eGpu, snaps.end());
    EXPECT_EQ(eGpu->name, "Hot-plugged GPU");
}

// #1116: a GPU gone from the re-enumerated list is dropped from the published
// GPU info, and its history records a gap from then on (it is forgotten once it
// leaves the window).
TEST(GPUModelTest, ReEnumerationDropsARemovedGpuAndItsHistoryGaps)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withGPU("GPU1", "Removable GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    rawProbe->withoutGPU("GPU1").withRescanReportingChange();
    model.refresh();

    const auto publication = model.publication();
    ASSERT_EQ(publication->gpuInfo.size(), 1U);
    EXPECT_EQ(publication->gpuInfo[0].id, "GPU0");
    const auto removed = publishedHistory(model, "GPU1").utilization;
    ASSERT_EQ(removed.size(), 2U);
    EXPECT_FALSE(std::isnan(removed[0]));
    EXPECT_TRUE(std::isnan(removed[1]));
    EXPECT_EQ(publishedHistory(model, "GPU0").utilization.size(), 2U);
}

// #1289: an adapter enumerated asleep has no sensor set of its own; once the
// probe has found it (and reports the change), the publication carries it, so
// the UI stops drawing the probe-wide charts.
TEST(GPUModelTest, ReEnumerationPublishesAWokenAdaptersSensorSet)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("dGPU", "Sleepy GPU", "NVIDIA");

    Domain::GPUModel model(std::move(probe));
    model.refresh();
    ASSERT_EQ(model.publication()->gpuInfo.size(), 1U);
    EXPECT_FALSE(model.publication()->gpuInfo[0].sensorCapabilities.has_value());

    Platform::GPUCapabilities ownSensors;
    ownSensors.hasTemperature = true;
    ownSensors.hasClockSpeeds = true;
    rawProbe->withSensorCapabilities("dGPU", ownSensors).withRescanReportingChange();
    model.refresh();

    const auto publication = model.publication();
    ASSERT_EQ(publication->gpuInfo.size(), 1U);
    ASSERT_TRUE(publication->gpuInfo[0].sensorCapabilities.has_value());
    const auto published = publication->gpuInfo[0].sensorCapabilities.value_or(Platform::GPUCapabilities{});
    EXPECT_TRUE(published.hasTemperature);
    EXPECT_FALSE(published.hasPowerMetrics);
    EXPECT_FALSE(published.hasFanSpeed);
}

// The capabilities are re-read with the GPU info: a probe that gains (or loses)
// a vendor library on a re-init reports so.
TEST(GPUModelTest, ReEnumerationRefreshesTheCapabilities)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    EXPECT_FALSE(model.capabilities().hasPerProcessMetrics);

    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    rawProbe->withCapabilities(caps).withProcessGPU(42, "GPU0", 1024).withRescanReportingChange();
    model.refresh();

    EXPECT_TRUE(model.capabilities().hasPerProcessMetrics);
    EXPECT_TRUE(model.publication()->capabilities.hasPerProcessMetrics);
    EXPECT_EQ(model.readProcessGPUCounters().size(), 1U);
}

// The reverse: a probe that loses per-process support on a re-init stops being asked
// for per-process counters, because readProcessGPUCounters()'s lock-free early exit
// follows the re-read capabilities (#1322).
TEST(GPUModelTest, ReEnumerationThatLosesPerProcessSupportSkipsTheProbe)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withCapabilities(caps).withProcessGPU(42, "GPU0", 1024);

    Domain::GPUModel model(std::move(probe));
    EXPECT_EQ(model.readProcessGPUCounters().size(), 1U);

    caps.hasPerProcessMetrics = false;
    rawProbe->withCapabilities(caps).withRescanReportingChange();
    model.refresh();
    const auto callsBefore = rawProbe->readProcessCountersCallCount();

    EXPECT_FALSE(model.capabilities().hasPerProcessMetrics);
    EXPECT_TRUE(model.readProcessGPUCounters().empty());
    EXPECT_EQ(rawProbe->readProcessCountersCallCount(), callsBefore);
}

// A startup enumeration that failed is retried at the full-rescan rate, so one
// failed query doesn't leave the tab saying "GPU monitoring is not available"
// for the whole session.
TEST(GPUModelTest, FailedStartupEnumerationIsRetriedOnTheNextFullRescan)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor").withEnumerationThrowing();

    Domain::GPUModel model(std::move(probe));
    const auto start = std::chrono::ceil<std::chrono::seconds>(std::chrono::steady_clock::now());
    model.refreshAt(start + std::chrono::seconds(1));
    EXPECT_FALSE(model.publication()->gpuInfoKnown);

    rawProbe->withEnumerationSucceeding();
    model.refreshAt(start + std::chrono::seconds(2)); // a quick rescan: no retry yet
    EXPECT_FALSE(model.publication()->gpuInfoKnown);

    model.refreshAt(start + std::chrono::seconds(Domain::Sampling::GPU_RESCAN_INTERVAL_SECONDS + 1));
    const auto publication = model.publication();
    EXPECT_TRUE(publication->gpuInfoKnown);
    ASSERT_EQ(publication->gpuInfo.size(), 1U);
    EXPECT_EQ(publication->gpuInfo[0].id, "GPU0");
}

// A re-enumeration that fails keeps the GPU info already known rather than
// publishing an empty list.
TEST(GPUModelTest, FailedReEnumerationKeepsThePreviousGpuInfo)
{
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withGPU("GPU0", "Test GPU", "TestVendor");

    Domain::GPUModel model(std::move(probe));
    rawProbe->withEnumerationThrowing().withRescanReportingChange();
    model.refresh();

    const auto publication = model.publication();
    EXPECT_TRUE(publication->gpuInfoKnown);
    ASSERT_EQ(publication->gpuInfo.size(), 1U);
    EXPECT_EQ(publication->gpuInfo[0].id, "GPU0");
    EXPECT_EQ(publication->snapshots.size(), 1U); // and keeps sampling
}

// =============================================================================
// Ordering (#1163)
// =============================================================================

std::vector<std::string> idsOf(const std::vector<Domain::GPUSnapshot>& snapshots)
{
    std::vector<std::string> ids;
    ids.reserve(snapshots.size());
    for (const auto& snapshot : snapshots)
    {
        ids.push_back(snapshot.gpuId);
    }
    return ids;
}

// Enough GPUs, in an order that is neither sorted nor insertion-hashed, that a hash-ordered map
// would not happen to preserve it.
std::vector<std::string> scrambledGpuIds()
{
    return {
        "GPU-7",
        "GPU-2",
        "GPU-11",
        "GPU-0",
        "GPU-9",
        "GPU-4",
        "GPU-13",
        "GPU-1",
        "GPU-8",
        "GPU-5",
        "GPU-12",
        "GPU-3",
    };
}

TEST(GPUModelTest, PublishedSnapshotsFollowEnumerationOrder)
{
    // The snapshots used to come out in the hash map's order, so the GPU tab's order (and the UI state
    // keyed by position) could change whenever the set of GPUs read did (#1163).
    const std::vector<std::string> enumerationOrder = scrambledGpuIds();
    auto probe = std::make_unique<MockGPUProbe>();
    auto* rawProbe = probe.get();
    for (const auto& id : enumerationOrder)
    {
        rawProbe->withGPU(id, "GPU " + id);
    }

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(idsOf(model.publication()->snapshots), enumerationOrder);
    EXPECT_EQ(idsOf(model.snapshots()), enumerationOrder);

    // A GPU missing from a read leaves the others in their order.
    rawProbe->withoutGPUCounters("GPU-9").withoutGPUCounters("GPU-7");
    model.refresh();

    std::vector<std::string> expected = enumerationOrder;
    std::erase(expected, "GPU-9");
    std::erase(expected, "GPU-7");
    EXPECT_EQ(idsOf(model.publication()->snapshots), expected);
    EXPECT_EQ(idsOf(model.snapshots()), expected);
}

TEST(GPUModelTest, UnenumeratedGpusFollowTheEnumeratedOnesSortedById)
{
    auto probe = std::make_unique<MockGPUProbe>();
    probe->withGPU("GPU-B", "Second").withGPU("GPU-A", "First");
    for (const char* lateId : {"late-9", "late-3", "late-7", "late-1", "late-5", "late-2", "late-8", "late-4"})
    {
        probe->withGPUCounters(lateId, makeGPUCounters(lateId));
    }

    Domain::GPUModel model(std::move(probe));
    model.refresh();

    const std::vector<std::string> expected = {
        "GPU-B",
        "GPU-A",
        "late-1",
        "late-2",
        "late-3",
        "late-4",
        "late-5",
        "late-7",
        "late-8",
        "late-9",
    };
    EXPECT_EQ(idsOf(model.publication()->snapshots), expected);
    EXPECT_EQ(idsOf(model.snapshots()), expected);
}

TEST(GPUModelTest, OrderSnapshotsByEnumerationEmitsADuplicatedIdOnce)
{
    Domain::GPUSnapshotMap snapshots;
    snapshots["GPU1"].gpuId = "GPU1";
    snapshots["GPU0"].gpuId = "GPU0";
    const std::vector<Platform::GPUInfo> gpuInfo = {makeGPUInfo("GPU1", "a"), makeGPUInfo("GPU0", "b"), makeGPUInfo("GPU1", "c")};

    EXPECT_EQ(idsOf(Domain::orderSnapshotsByEnumeration(gpuInfo, snapshots)), (std::vector<std::string>{"GPU1", "GPU0"}));
    // Enumeration failed: no GPU is listed, so all of them are ordered by id.
    EXPECT_EQ(idsOf(Domain::orderSnapshotsByEnumeration({}, snapshots)), (std::vector<std::string>{"GPU0", "GPU1"}));
}

// =============================================================================
// Publication Handoff (#868)
// =============================================================================

/// A probe enumerating and reporting `gpuCount` GPUs named GPU0..GPUn-1.
[[nodiscard]] std::unique_ptr<MockGPUProbe> makeManyGpus(std::size_t gpuCount)
{
    auto probe = std::make_unique<MockGPUProbe>();
    for (std::size_t i = 0; i < gpuCount; ++i)
    {
        probe->withGPU("GPU" + std::to_string(i), "Test GPU " + std::to_string(i));
    }
    return probe;
}

/// Every GPU's series in one generation is aligned to its timestamps: a truncated or torn copy of any
/// one of them fails. Once there is history, every GPU of `gpuCount` has its history and snapshot.
[[nodiscard]] bool isAligned(const Domain::GPUPublication& publication, std::size_t gpuCount)
{
    const auto historyAligned = [](const auto& entry)
    {
        const Domain::GPUPublishedHistory& history = entry.second;
        const std::size_t n = history.timestamps.size();
        return history.memoryUsedBytes.size() == n && history.memoryTotalBytes.size() == n && history.utilization.size() == n &&
               history.memoryPercent.size() == n && history.gpuClock.size() == n && history.encoder.size() == n &&
               history.decoder.size() == n && history.temperature.size() == n && history.power.size() == n && history.fanSpeed.size() == n;
    };
    const bool complete =
        publication.version == 0 || (publication.histories.size() == gpuCount && publication.snapshots.size() == gpuCount);
    return complete && std::ranges::all_of(publication.histories, historyAligned);
}

TEST(GPUModelTest, PublicationDoesNotWaitForTheWriterToCopyHistory)
{
    // #868: refreshAt() copied every GPU's history ring into the new publication while holding the
    // lock publication() needs, so a UI-thread read landing then waited for most of the write -- one
    // slow read per generation. With the copy outside the lock a read waits for a pointer swap at
    // most. Enough GPUs and history that the copy dominates a write; see PublicationLatency.h.
    constexpr std::size_t GPUS = 64;
    constexpr std::size_t PREFILL_SAMPLES = 300;
    constexpr std::size_t WRITES = 40;
    constexpr auto STEP = std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS);

    Domain::GPUModel model(makeManyGpus(GPUS));
    auto now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    for (std::size_t i = 0; i < PREFILL_SAMPLES; ++i)
    {
        model.refreshAt(now += STEP);
    }

    const auto result = TestPublication::measure(
        WRITES,
        [&](std::size_t) { model.refreshAt(now += STEP); },
        [&] { return model.publication(); },
        [&] { return model.publicationVersion(); },
        [](const Domain::GPUPublication& publication) { return isAligned(publication, GPUS); });

    EXPECT_EQ(result.versionRegressions, 0U);
    EXPECT_EQ(result.versionAheadOfPointer, 0U);
    EXPECT_EQ(result.inconsistentReads, 0U);
    ASSERT_FALSE(result.pacingTimedOut) << "the reader stopped keeping up with the writer";
    EXPECT_EQ(model.publicationVersion(), PREFILL_SAMPLES + result.totalWrites);
    EXPECT_GT(result.reads, WRITES); // the pacing guarantees a read per write, plus the last one
    // Only reads that land inside a write can show contention; see PublicationLatency.h.
    ASSERT_TRUE(result.overlapAchieved) << "only " << result.overlappingReads << " reads started during a write after " << result.trials
                                        << " trials (need " << TestPublication::MIN_OVERLAPPING_READS
                                        << "): the scheduler never ran the reader alongside the writer, so contention wasn't measured";
    // Before #868 about one read per write waited out the copy. A quarter allows for scheduler noise.
    EXPECT_LE(result.slowReads, WRITES / 4) << "median write " << result.medianWriteMs << " ms, slowest read " << result.maxReadMs
                                            << " ms over " << result.reads << " reads, " << result.slowOverlappingReads << " slow of "
                                            << result.overlappingReads << " overlapping";
}

TEST(GPUModelTest, ConcurrentWritersPublishEveryGenerationInOrder)
{
    // The sampler thread (refreshAt) and the UI thread (setMaxHistorySeconds) both publish. They are
    // serialised, so generations are committed in version order with none lost, and a reader never
    // sees a regressed or misaligned one (#868). Both writers are paced on the reader (ReadPacer), so
    // reads really interleave with the publishing however the threads are scheduled.
    constexpr std::size_t GPUS = 4;
    constexpr int SAMPLES = 300;
    constexpr int RESIZES = 300;
    constexpr auto STEP = std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS);
    const auto start = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);

    Domain::GPUModel model(makeManyGpus(GPUS));
    model.refreshAt(start); // publish once so resizes republish

    TestPublication::ReadPacer pacer;
    std::atomic<int> writersRunning{2};
    std::atomic<std::size_t> readsWhilePublishing{0};
    std::atomic<bool> stop{false};
    std::thread sampler(
        [&]
        {
            std::size_t lastRead = 0;
            auto now = start;
            for (int i = 0; i < SAMPLES && pacer.awaitReadSince(lastRead); ++i)
            {
                model.refreshAt(now += STEP);
            }
            writersRunning.fetch_sub(1);
        });
    std::thread resizer(
        [&]
        {
            std::size_t lastRead = 0;
            for (int i = 0; i < RESIZES && pacer.awaitReadSince(lastRead); ++i)
            {
                model.setMaxHistorySeconds((i % 2 == 0) ? Domain::Sampling::HISTORY_SECONDS_MIN
                                                        : Domain::Sampling::HISTORY_SECONDS_DEFAULT);
            }
            writersRunning.fetch_sub(1);
        });
    std::thread reader(
        [&]
        {
            std::uint64_t lastSeen = 0;
            while (!stop.load())
            {
                const std::uint64_t announced = model.publicationVersion();
                const auto publication = model.publication();
                EXPECT_GE(publication->version, lastSeen);
                EXPECT_GE(publication->version, announced);
                EXPECT_TRUE(isAligned(*publication, GPUS));
                lastSeen = publication->version;
                if (writersRunning.load() > 0)
                {
                    readsWhilePublishing.fetch_add(1);
                }
                pacer.readDone();
            }
        });
    sampler.join();
    resizer.join();
    stop.store(true);
    reader.join();

    ASSERT_FALSE(pacer.timedOut()) << "the reader stopped keeping up with the writers";
    // Every sample and every resize waited for a fresh read, made while that writer was still running.
    EXPECT_GE(readsWhilePublishing.load(), static_cast<std::size_t>(std::max(SAMPLES, RESIZES)));

    EXPECT_EQ(model.publicationVersion(), static_cast<std::uint64_t>(1 + SAMPLES + RESIZES));
    EXPECT_EQ(model.publication()->version, model.publicationVersion());
    // Each GPU's history ends at the last sample: no sample was lost to a concurrent republish.
    const auto publication = model.publication();
    const double lastSeconds = std::chrono::duration<double>((start + (STEP * SAMPLES)).time_since_epoch()).count();
    for (const auto& [gpuId, history] : publication->histories)
    {
        SCOPED_TRACE(gpuId);
        ASSERT_FALSE(history.timestamps.empty());
        EXPECT_DOUBLE_EQ(history.timestamps.back(), lastSeconds);
    }
}

TEST(GPUModelTest, SetMaxHistorySecondsDoesNotWaitForASlowProbeRead)
{
    // The writer lock that serialises publishes (#868) is taken only after refreshAt() has released
    // the probe lock, so a probe read stuck in the driver never holds up a window change on the UI
    // thread, as it did not before #868. Hold a refresh blocked inside the probe read and resize.
    auto probe = makeManyGpus(1);
    auto* rawProbe = probe.get();
    Domain::GPUModel model(std::move(probe));
    model.refresh(); // publish once, so the resize republishes
    const std::uint64_t versionBefore = model.publicationVersion();
    rawProbe->armBlockingReadGPUCounters();

    std::thread blockedRefresh([&model] { model.refresh(); });
    EXPECT_TRUE(waitForBlockedEntry(*rawProbe)) << "background refresh() never entered its blocking probe call within the deadline";

    // On its own thread with a bounded wait, so a regression fails instead of hanging the binary.
    auto future = std::async(std::launch::async, [&model] { model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MIN); });
    const auto status = future.wait_for(std::chrono::milliseconds(500));
    EXPECT_EQ(status, std::future_status::ready) << "setMaxHistorySeconds() waited for the blocked probe read";
    if (status == std::future_status::ready)
    {
        EXPECT_EQ(model.publicationVersion(), versionBefore + 1); // the resize republished at once (#1145)
    }

    rawProbe->releaseBlockedReadGPUCounters();
    blockedRefresh.join();
    future.wait();
    EXPECT_EQ(model.publicationVersion(), versionBefore + 2);
}

// =============================================================================
// Shared history (#1412)
// =============================================================================

/// Every series of one GPU's published history is as long as its timestamps.
[[nodiscard]] bool seriesAligned(const Domain::GPUPublishedHistory& history)
{
    const std::size_t samples = history.timestamps.size();
    return history.memoryUsedBytes.size() == samples && history.memoryTotalBytes.size() == samples &&
           history.utilization.size() == samples && history.memoryPercent.size() == samples && history.gpuClock.size() == samples &&
           history.encoder.size() == samples && history.decoder.size() == samples && history.temperature.size() == samples &&
           history.power.size() == samples && history.fanSpeed.size() == samples;
}

/// Every sample of every series of @p publication, copied out by bit pattern (NaN gaps compare equal to
/// themselves), keyed "<gpu>/<series>", so a later comparison sees whether any changed.
[[nodiscard]] std::map<std::string, std::vector<std::uint64_t>> publishedValues(const Domain::GPUPublication& publication)
{
    std::map<std::string, std::vector<std::uint64_t>> values;
    for (const auto& [gpuId, history] : publication.histories)
    {
        auto& timestamps = values[gpuId + "/timestamps"];
        for (const double timestamp : history.timestamps)
        {
            timestamps.push_back(std::bit_cast<std::uint64_t>(timestamp));
        }
        values[gpuId + "/memoryUsedBytes"].assign(history.memoryUsedBytes.begin(), history.memoryUsedBytes.end());
        values[gpuId + "/memoryTotalBytes"].assign(history.memoryTotalBytes.begin(), history.memoryTotalBytes.end());
        const std::vector<std::pair<std::string, const Domain::HistoryView<float>*>> floatSeries{
            {"utilization", &history.utilization},
            {"memoryPercent", &history.memoryPercent},
            {"gpuClock", &history.gpuClock},
            {"encoder", &history.encoder},
            {"decoder", &history.decoder},
            {"temperature", &history.temperature},
            {"power", &history.power},
            {"fanSpeed", &history.fanSpeed},
        };
        for (const auto& [name, series] : floatSeries)
        {
            std::string key = gpuId;
            key += '/';
            key += name;
            auto& out = values[key];
            for (const float value : *series)
            {
                out.push_back(std::bit_cast<std::uint32_t>(value));
            }
        }
    }
    return values;
}

/// A probe and model sampled once a simulated second (refreshAt()), whose GPUs come and go on rescans.
struct SteppedGpuModel
{
    MockGPUProbe* probe = nullptr; // owned by model
    std::unique_ptr<Domain::GPUModel> model;
    std::chrono::steady_clock::time_point start = std::chrono::ceil<std::chrono::seconds>(std::chrono::steady_clock::now());
    int step = 0;

    SteppedGpuModel()
    {
        auto owned = std::make_unique<MockGPUProbe>();
        owned->withGPU("GPU0", "GPU Zero").withGPU("GPU1", "GPU One");
        probe = owned.get();
        model = std::make_unique<Domain::GPUModel>(std::move(owned));
    }

    /// One refresh @p secondsPerStep after the last, with every GPU's utilization changed.
    void sample(double secondsPerStep = 1.0)
    {
        for (const char* gpuId : {"GPU0", "GPU1", "GPU2"})
        {
            probe->withUtilization(gpuId, static_cast<double>((step * 7) % 100));
        }
        const auto offset = std::chrono::duration<double>(static_cast<double>(step) * secondsPerStep);
        model->refreshAt(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(offset));
        ++step;
    }
};

// A publication shares its samples with the model rather than copying them, so it must stay exactly as
// published while the model appends, trims (by time and by a window change), moves its samples to new blocks,
// and gains and loses GPUs on rescans.
TEST(GPUModelTest, OlderPublicationIsUnchangedByLaterSamplesRescansAndTrims)
{
    SteppedGpuModel stepped;
    stepped.model->setMaxHistorySeconds(60.0);
    while (stepped.step < 100)
    {
        stepped.sample();
    }
    const auto old = stepped.model->publication();
    const auto expected = publishedValues(*old);
    ASSERT_EQ(old->histories.size(), 2U);
    ASSERT_GT(old->histories.at("GPU0").timestamps.size(), 50U);

    while (stepped.step < 1000)
    {
        if (stepped.step == 300)
        {
            stepped.probe->withGPU("GPU2", "GPU Two").withRescanReportingChange();
        }
        if (stepped.step == 400)
        {
            stepped.probe->withoutGPU("GPU1").withRescanReportingChange();
        }
        if (stepped.step == 500)
        {
            stepped.model->setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MIN);
        }
        stepped.sample();
        const auto latest = stepped.model->publication();
        ASSERT_TRUE(std::ranges::all_of(latest->histories, [](const auto& entry) { return seriesAligned(entry.second); }))
            << "step " << stepped.step;
    }
    EXPECT_EQ(publishedValues(*old), expected);

    const auto latest = stepped.model->publication();
    EXPECT_TRUE(latest->histories.contains("GPU0"));
    EXPECT_TRUE(latest->histories.contains("GPU2"));
    EXPECT_FALSE(latest->histories.contains("GPU1")); // gone for longer than the window: forgotten
}

// Consecutive publications share their history (an O(series) publish, not O(history x series)), and every
// series stays aligned with its GPU's timestamps.
TEST(GPUModelTest, ConsecutivePublicationsShareTheirHistory)
{
    SteppedGpuModel stepped;
    stepped.model->setMaxHistorySeconds(120.0);
    while (stepped.step < 300)
    {
        stepped.sample();
    }

    constexpr int GENERATIONS = 200;
    int shared = 0;
    auto previous = stepped.model->publication();
    for (int i = 0; i < GENERATIONS; ++i)
    {
        stepped.sample();
        const auto next = stepped.model->publication();
        ASSERT_EQ(next->histories.size(), 2U);
        const auto& gpu0 = next->histories.at("GPU0");
        const auto& gpu1 = next->histories.at("GPU1");
        ASSERT_TRUE(seriesAligned(gpu0));
        ASSERT_TRUE(seriesAligned(gpu1));
        const auto& before0 = previous->histories.at("GPU0");
        const auto& before1 = previous->histories.at("GPU1");
        if (gpu0.timestamps.sharesStorageWith(before0.timestamps) && gpu0.utilization.sharesStorageWith(before0.utilization) &&
            gpu1.fanSpeed.sharesStorageWith(before1.fanSpeed))
        {
            ++shared;
            // The same samples in place: the newer view starts one sample later in the same memory.
            EXPECT_EQ(gpu0.timestamps.data(), before0.timestamps.data() + 1);
            EXPECT_EQ(gpu0.utilization.data(), before0.utilization.data() + 1);
        }
        previous = next;
    }
    // Only a compaction into a new block -- about once per window's worth of samples -- copies.
    EXPECT_GE(shared, GENERATIONS - 4);
}

// A UI-style reader walks every sample of the latest publication while the sampler appends, trims, compacts,
// and adds and drops GPUs underneath it. Under TSan this shows the reader never reads a slot the writer
// writes: published samples are never written again (#1412). The sampler is paced on the reader
// (TestPublication::ReadPacer): it starts only once the reader has completed a traversal, and waits for a
// new one every PACE_EVERY samples, so the reads really overlap the sampling however the threads are scheduled.
TEST(GPUModelTest, ReadingPublishedHistoryWhileSamplingIsRaceFree)
{
    constexpr int SAMPLES = 2000;
    constexpr int PACE_EVERY = 10;
    SteppedGpuModel stepped;
    stepped.model->setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MIN);
    stepped.sample(0.25);

    TestPublication::ReadPacer pacer;
    std::atomic<bool> done{false};
    std::atomic<std::size_t> misaligned{0};
    std::thread reader(
        [&]
        {
            while (!done.load(std::memory_order_acquire))
            {
                const auto publication = stepped.model->publication();
                const auto values = publishedValues(*publication); // reads every sample of every series
                for (const auto& [gpuId, history] : publication->histories)
                {
                    const auto& timestamps = values.at(gpuId + "/timestamps");
                    for (const auto& [key, series] : values)
                    {
                        if (key.starts_with(gpuId + "/") && series.size() != timestamps.size())
                        {
                            misaligned.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    for (std::size_t i = 1; i < history.timestamps.size(); ++i)
                    {
                        if (history.timestamps[i] <= history.timestamps[i - 1])
                        {
                            misaligned.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                pacer.readDone(); // one full traversal of a publication
            }
        });

    // Start sampling only once the reader is running, then count the traversals made while sampling.
    std::size_t readsSeen = 0;
    const bool readerStarted = pacer.awaitReadSince(readsSeen);
    const std::size_t readsBeforeSampling = readsSeen;
    int paced = 0;
    for (int i = 0; readerStarted && i < SAMPLES; ++i)
    {
        if (i % PACE_EVERY == 0)
        {
            if (!pacer.awaitReadSince(readsSeen))
            {
                break;
            }
            ++paced;
        }
        // GPU2 comes and goes: present for 100 samples of every 300, so it is added, gapped and forgotten.
        if (i % 300 == 100)
        {
            stepped.probe->withGPU("GPU2", "GPU Two").withRescanReportingChange();
        }
        if (i % 300 == 200)
        {
            stepped.probe->withoutGPU("GPU2").withRescanReportingChange();
        }
        stepped.sample(0.25);
    }
    done.store(true, std::memory_order_release);
    reader.join();
    ASSERT_TRUE(readerStarted) << "the reader never completed a traversal";
    ASSERT_FALSE(pacer.timedOut()) << "the reader stopped making progress while sampling";
    // Every paced wait saw a traversal completed after the previous one, while sampling was under way.
    EXPECT_EQ(paced, SAMPLES / PACE_EVERY);
    EXPECT_GE(readsSeen - readsBeforeSampling, static_cast<std::size_t>(SAMPLES / PACE_EVERY));
    EXPECT_EQ(misaligned.load(), 0U);
}

} // namespace
