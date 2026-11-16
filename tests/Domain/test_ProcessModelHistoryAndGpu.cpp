/// @file test_ProcessModelHistoryAndGpu.cpp
/// @brief Domain::ProcessModel: the system-level histories, memory breakdown, peak RSS, GPU data merging and ownership, and snapshot
/// versions. Split from test_ProcessModel.cpp by feature (#3850); the shared helpers are in ProcessModelTestSupport.h.

#include "Domain/GPUModel.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/GPUTypes.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ImageSignature.h"
#include "Platform/ProcessTypes.h"
#include "Platform/SchedPolicy.h"
#include "ProcessModelTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Use shared mock from TestMocks namespace
using ProcessModelTesting::makeCounter;
using ProcessModelTesting::ManualClock;
using ProcessModelTesting::OVERFLOW_TEST_MARGIN;
using ProcessModelTesting::systemHistories;
using TestMocks::makeProcessCounters;
using TestMocks::MockProcessProbe;

// =============================================================================
// System-Level History Tests (for untested functions)
// =============================================================================

TEST(ProcessModelTest, SystemHistoriesAreEmptyInitially)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    const auto histories = systemHistories(model);
    EXPECT_TRUE(histories.timestamps.empty());
    EXPECT_TRUE(histories.pageFaults.empty());
    EXPECT_TRUE(histories.threadCount.empty());
    EXPECT_TRUE(histories.handleCount.empty());
    EXPECT_TRUE(histories.power.empty());
}

TEST(ProcessModelTest, SystemHandleCountHistoryAggregatesAcrossProcesses)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setTotalCpuTime(100000);

    auto* rawProbe = probe.get();
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());

    // First sample — no history entry yet (needs two samples for a delta)
    auto c1 = makeCounter(100, "proc_a", 'R', 1000, 500);
    c1.handleCount = 10;
    auto c2 = makeCounter(200, "proc_b", 'R', 2000, 1000);
    c2.handleCount = 25;
    rawProbe->setCounters({c1, c2});
    model.refresh();

    clock.advance(std::chrono::milliseconds(10));

    c1.userTime += 100;
    c2.userTime += 100;
    rawProbe->setCounters({c1, c2});
    model.refresh();

    const auto history = systemHistories(model).handleCount;
    ASSERT_FALSE(history.empty());
    // Aggregated handle count should be the sum: 10 + 25 = 35
    EXPECT_DOUBLE_EQ(history.back(), 35.0);
}

TEST(ProcessModelTest, SystemHandleCountHistoryAlignedWithTimestamps)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setTotalCpuTime(100000);

    auto* rawProbe = probe.get();
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());

    auto counter = makeCounter(100, "proc_a", 'R', 1000, 500);
    counter.handleCount = 5;
    rawProbe->setCounters({counter});
    model.refresh();

    clock.advance(std::chrono::milliseconds(10));

    counter.userTime += 100;
    rawProbe->setCounters({counter});
    model.refresh();

    const auto histories = systemHistories(model);
    EXPECT_EQ(histories.timestamps.size(), histories.handleCount.size());
}

TEST(ProcessModelTest, HistoryRetentionBelowTheMinimumIsClamped)
{
    // Drives sample time via the injectable clock instead of a real sleep, so the
    // pushed history entries get distinct, deterministic timestamps regardless of
    // scheduler/clock-resolution timing.
    auto currentTime = Domain::ProcessModel::Clock::time_point{};
    Domain::ProcessModel model(nullptr, [&currentTime] { return currentTime; });
    const auto counter = makeCounter(100, "history_proc", 'R', 1000, 500);

    // The first sample only seeds the deltas; each later one adds a history entry: t = 5, 10, ..., 25.
    std::uint64_t totalCpu = 100000;
    model.updateFromCounters({counter}, totalCpu);
    for (int i = 0; i < 5; ++i)
    {
        currentTime += std::chrono::seconds(5);
        totalCpu += 100000;
        model.updateFromCounters({counter}, totalCpu);
    }
    ASSERT_EQ(systemHistories(model).timestamps.size(), 5U);

    // Clamped to HISTORY_SECONDS_MIN like every other model (#1145), where it used to keep a
    // zero-second window and with it only the current sample: t = 15, 20, 25, plus t = 10 kept just
    // before the cutoff (#1016).
    static_assert(Domain::Sampling::HISTORY_SECONDS_MIN == 10, "the expected count below assumes a 10 s minimum");
    model.setMaxHistorySeconds(0.0);
    EXPECT_EQ(systemHistories(model).timestamps.size(), 4U);
}

// #1145: a window change trims the aggregated system histories at once and hands them out as a new
// generation, instead of the Overview charts keeping the old window until the next sample. The
// snapshot generation is unchanged: the process list did not change.
TEST(ProcessModelTest, ShrinkingTheHistoryWindowPublishesTheTrimmedSystemHistories)
{
    auto currentTime = Domain::ProcessModel::Clock::time_point{};
    Domain::ProcessModel model(nullptr, [&currentTime] { return currentTime; });
    const auto counter = makeCounter(100, "history_proc", 'R', 1000, 500);

    Domain::ProcessSystemHistories histories;
    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_DEFAULT);
    EXPECT_FALSE(model.tryCopySystemHistoriesIfNewer(0, histories)); // Nothing published yet

    // t = 5, 10, ..., 60 (the first sample only seeds the deltas).
    std::uint64_t totalCpu = 100000;
    model.updateFromCounters({counter}, totalCpu);
    for (int i = 0; i < 12; ++i)
    {
        currentTime += std::chrono::seconds(5);
        totalCpu += 100000;
        model.updateFromCounters({counter}, totalCpu);
    }
    ASSERT_TRUE(model.tryCopySystemHistoriesIfNewer(0, histories));
    ASSERT_EQ(histories.timestamps.size(), 12U);
    const std::uint64_t historyVersion = histories.version;
    const std::uint64_t snapshotVersion = model.snapshotVersion();

    model.setMaxHistorySeconds(Domain::Sampling::HISTORY_SECONDS_MIN);

    ASSERT_TRUE(model.tryCopySystemHistoriesIfNewer(historyVersion, histories));
    EXPECT_GT(histories.version, historyVersion);
    // t = 50, 55, 60, plus t = 45 kept just before the cutoff (#1016).
    EXPECT_EQ(histories.timestamps.size(), 4U);
    EXPECT_EQ(histories.power.size(), 4U);
    EXPECT_EQ(histories.threadCount.size(), 4U);
    EXPECT_EQ(model.snapshotVersion(), snapshotVersion);
    EXPECT_FALSE(model.tryCopySystemHistoriesIfNewer(histories.version, histories));
}

// #2156: each generation of the aggregated system histories shares the previous one's storage
// instead of deep-copying the whole window, and a generation a reader holds never changes after it.
TEST(ProcessModelTest, ConsecutiveSystemHistoryGenerationsShareStorageInsteadOfCopying)
{
    auto currentTime = Domain::ProcessModel::Clock::time_point{};
    Domain::ProcessModel model(nullptr, [&currentTime] { return currentTime; });
    auto counter = makeCounter(100, "history_proc", 'R', 1000, 500);
    counter.threadCount = 3;

    std::uint64_t totalCpu = 100000;
    const auto sampleOnce = [&]
    {
        currentTime += std::chrono::seconds(1);
        totalCpu += 100000;
        ++counter.threadCount;
        model.updateFromCounters({counter}, totalCpu);
    };
    sampleOnce(); // seeds the deltas: no history sample yet
    sampleOnce();

    Domain::ProcessSystemHistories previous;
    ASSERT_TRUE(model.tryCopySystemHistoriesIfNewer(0, previous));
    ASSERT_EQ(previous.timestamps.size(), 1U);

    // Fewer samples than a shared block's smallest size, so no compaction moves them in between.
    constexpr int GENERATIONS = 20;
    for (int generation = 0; generation < GENERATIONS; ++generation)
    {
        SCOPED_TRACE("generation " + std::to_string(generation));
        const std::vector<double> previousThreads(previous.threadCount.begin(), previous.threadCount.end());
        const std::vector<double> previousTimestamps(previous.timestamps.begin(), previous.timestamps.end());

        sampleOnce();
        Domain::ProcessSystemHistories next;
        ASSERT_TRUE(model.tryCopySystemHistoriesIfNewer(previous.version, next));
        ASSERT_EQ(next.timestamps.size(), previous.timestamps.size() + 1);

        // The same storage, not a copy of it, for every series.
        EXPECT_TRUE(next.timestamps.sharesStorageWith(previous.timestamps));
        EXPECT_TRUE(next.power.sharesStorageWith(previous.power));
        EXPECT_TRUE(next.pageFaults.sharesStorageWith(previous.pageFaults));
        EXPECT_TRUE(next.threadCount.sharesStorageWith(previous.threadCount));
        EXPECT_TRUE(next.handleCount.sharesStorageWith(previous.handleCount));
        // The new generation starts at the same samples, so nothing was rebuilt for it ...
        EXPECT_EQ(next.timestamps.data(), previous.timestamps.data());
        EXPECT_EQ(next.threadCount.data(), previous.threadCount.data());
        // ... and the generation held from before is unchanged by the append.
        EXPECT_EQ(std::vector<double>(previous.threadCount.begin(), previous.threadCount.end()), previousThreads);
        EXPECT_EQ(std::vector<double>(previous.timestamps.begin(), previous.timestamps.end()), previousTimestamps);
        EXPECT_DOUBLE_EQ(next.threadCount.back(), static_cast<double>(counter.threadCount));

        previous = std::move(next);
    }
}

// =============================================================================
// Memory Breakdown (Private and Swap columns, #1879, #1885)
// =============================================================================

TEST(ProcessModelTest, PrivateAndSwapBytesReachTheSnapshotAndFollowEachSample)
{
    // Each sample's figures are the probe's, copied as they are (Linux's RssAnon and VmSwap, #1885): a
    // process with no swap reads 0. Neither is a delta or kept as a peak.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setTotalCpuTime(100000);
    probe->withProcess(100, "unswapped_app")
        .withMemoryBreakdown(100, 96ULL * 1024 * 1024, 0)
        .withProcess(200, "swapped_app")
        .withMemoryBreakdown(200, 40ULL * 1024 * 1024, 8ULL * 1024 * 1024);
    auto* rawProbe = probe.get();
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    // A process's {privateBytes, swapBytes}, or both the maximum if it is missing.
    using Breakdown = std::pair<std::uint64_t, std::uint64_t>;
    const auto breakdown = [&model](std::int32_t pid) -> Breakdown
    {
        for (const auto& snap : model.snapshots())
        {
            if (snap.pid == pid)
            {
                return {snap.privateBytes, snap.swapBytes};
            }
        }
        constexpr std::uint64_t MISSING = std::numeric_limits<std::uint64_t>::max();
        return {MISSING, MISSING};
    };
    EXPECT_EQ(breakdown(100), Breakdown(96ULL * 1024 * 1024, 0));
    EXPECT_EQ(breakdown(200), Breakdown(40ULL * 1024 * 1024, 8ULL * 1024 * 1024));

    // A later sample replaces them, down as well as up.
    clock.advance(std::chrono::milliseconds(10));
    rawProbe->withMemoryBreakdown(100, 64ULL * 1024 * 1024, 0).withMemoryBreakdown(200, 48ULL * 1024 * 1024, 2ULL * 1024 * 1024);
    model.refresh();
    EXPECT_EQ(breakdown(100), Breakdown(64ULL * 1024 * 1024, 0));
    EXPECT_EQ(breakdown(200), Breakdown(48ULL * 1024 * 1024, 2ULL * 1024 * 1024));
}

// =============================================================================
// Peak RSS Tracking Tests
// =============================================================================

TEST(ProcessModelTest, AnOsPeakThatDecreasesKeepsTheHighestPeakSeen)
{
    // #1351 review: with hasPeakRss the OS peak (Linux VmHWM) is used, but VmHWM resets on exec while
    // the PID and start time stay the same; Peak Mem must not drop below a peak already observed.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setTotalCpuTime(100000);
    Platform::ProcessCapabilities caps = probe->capabilities();
    caps.hasPeakRss = true;
    probe->setCapabilities(caps);
    auto* rawProbe = probe.get();
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());

    auto counter = makeCounter(100, "proc1", 'R', 1000, 500, 1000, 10 * 1024 * 1024);
    counter.peakRssBytes = 50 * 1024 * 1024;
    rawProbe->setCounters({counter});
    model.refresh();
    ASSERT_EQ(model.snapshots().size(), 1U);
    EXPECT_EQ(model.snapshots()[0].peakMemoryBytes, 50U * 1024 * 1024);

    clock.advance(std::chrono::milliseconds(10));
    counter.peakRssBytes = 12 * 1024 * 1024; // exec reset VmHWM; same PID and start time
    rawProbe->setCounters({counter});
    model.refresh();
    EXPECT_EQ(model.snapshots()[0].peakMemoryBytes, 50U * 1024 * 1024) << "an observed peak is kept";

    clock.advance(std::chrono::milliseconds(10));
    counter.peakRssBytes = 80 * 1024 * 1024;
    rawProbe->setCounters({counter});
    model.refresh();
    EXPECT_EQ(model.snapshots()[0].peakMemoryBytes, 80U * 1024 * 1024);
}

TEST(ProcessModelTest, PeakRssTracksMaximumMemory)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setTotalCpuTime(100000);

    // Keep raw pointer for test control before moving unique_ptr
    auto* rawProbe = probe.get();
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());

    // Start with 10MB
    auto counter = makeCounter(100, "proc1", 'R', 1000, 500, 1000, 10 * 1024 * 1024);
    rawProbe->setCounters({counter});
    model.refresh();

    auto snaps1 = model.snapshots();
    ASSERT_EQ(snaps1.size(), 1);
    auto peak1 = snaps1[0].peakMemoryBytes;

    clock.advance(std::chrono::milliseconds(10));

    // Increase to 20MB
    counter.rssBytes = 20 * 1024 * 1024;
    counter.userTime += 100;
    rawProbe->setCounters({counter});
    model.refresh();

    auto snaps2 = model.snapshots();
    ASSERT_EQ(snaps2.size(), 1);
    auto peak2 = snaps2[0].peakMemoryBytes;

    EXPECT_GT(peak2, peak1);
    EXPECT_EQ(peak2, 20 * 1024 * 1024);

    clock.advance(std::chrono::milliseconds(10));

    // Decrease to 15MB - peak should stay at 20MB
    counter.rssBytes = 15 * 1024 * 1024;
    counter.userTime += 100;
    rawProbe->setCounters({counter});
    model.refresh();

    auto snaps3 = model.snapshots();
    ASSERT_EQ(snaps3.size(), 1);
    auto peak3 = snaps3[0].peakMemoryBytes;

    EXPECT_EQ(peak3, 20 * 1024 * 1024); // Peak should not decrease
}

TEST(ProcessModelTest, PageFaultRateCountsThroughTheProbesCounterWidth)
{
    // #1184: Windows keeps the page-fault count in a 32-bit ULONG. A wrap between samples is that
    // interval's faults, not a 0 rate; a 64-bit counter that goes down is still a reset.
    constexpr std::uint64_t MAX32 = 0xFFFF'FFFFULL;
    for (const std::uint8_t bits : {std::uint8_t{32}, std::uint8_t{64}})
    {
        SCOPED_TRACE(static_cast<int>(bits));
        auto probe = std::make_unique<MockProcessProbe>();
        probe->setTotalCpuTime(100000);
        probe->setCapabilities(Platform::ProcessCapabilities{.hasPageFaults = true, .pageFaultCountBits = bits});
        auto* rawProbe = probe.get();
        ManualClock clock;
        Domain::ProcessModel model(std::move(probe), clock.now());

        auto counter = makeCounter(100, "proc1", 'R', 1000, 500, 1000, 1024);
        counter.pageFaultCount = MAX32 - 99;
        rawProbe->setCounters({counter});
        model.refresh();

        clock.advance(std::chrono::seconds(1));
        counter.pageFaultCount = 100; // 200 faults later, past the 32-bit top
        counter.userTime += 100;
        rawProbe->setCounters({counter});
        rawProbe->setTotalCpuTime(200000);
        model.refresh();

        const auto snaps = model.snapshots();
        ASSERT_EQ(snaps.size(), 1U);
        EXPECT_DOUBLE_EQ(snaps[0].pageFaultsPerSec, bits == 32 ? 200.0 : 0.0);
    }
}

TEST(ProcessModelTest, PeakRssResetForNewProcess)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setTotalCpuTime(100000);

    // Keep raw pointer for test control before moving unique_ptr
    auto* rawProbe = probe.get();
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());

    // Process with PID 100
    auto counter1 = makeCounter(100, "proc1", 'R', 1000, 500, 1000, 20 * 1024 * 1024);
    rawProbe->setCounters({counter1});
    model.refresh();

    auto snaps1 = model.snapshots();
    ASSERT_EQ(snaps1.size(), 1);
    EXPECT_EQ(snaps1[0].peakMemoryBytes, 20 * 1024 * 1024);

    clock.advance(std::chrono::milliseconds(10));

    // New process with same PID but different start time (PID reuse)
    auto counter2 = makeCounter(100, "proc2", 'R', 500, 250, 2000, 5 * 1024 * 1024);
    rawProbe->setCounters({counter2});
    model.refresh();

    auto snaps2 = model.snapshots();
    ASSERT_EQ(snaps2.size(), 1);
    EXPECT_EQ(snaps2[0].peakMemoryBytes, 5 * 1024 * 1024); // Peak should reset for new process
}

// =============================================================================
// GPU Data Merging Tests
// =============================================================================

using TestMocks::MockGPUProbe;

TEST(ProcessModelTest, MergeGPUDataUpdatesProcessSnapshots)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Test GPU", "TestVendor").withProcessGPU(100, "GPU0", 512ULL * 1024 * 1024);

    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));

    // Set GPU model (shared ownership)
    processModel.setGPUModel(gpuModel);

    // GPUModel needs to be refreshed first to have process counters available
    processModel.refresh(); // Seed: the processes are listed before the GPU read, as in the app (#1417)
    gpuModel->refresh();

    // ProcessModel refresh will automatically merge GPU data
    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].gpuMemoryBytes, 512ULL * 1024 * 1024);
}

TEST(ProcessModelTest, MergeGPUDataMultipleProcesses)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "proc1", 'R', 1000, 500), makeCounter(200, "proc2", 'R', 2000, 1000)});
    processProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Test GPU", "TestVendor")
        .withProcessGPU(100, "GPU0", 256ULL * 1024 * 1024)
        .withProcessGPU(200, "GPU0", 128ULL * 1024 * 1024);

    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);

    processModel.refresh(); // Seed: the processes are listed before the GPU read, as in the app (#1417)
    gpuModel->refresh();
    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 2);

    // Find process 100
    auto it1 = std::find_if(snaps.begin(), snaps.end(), [](const auto& s) { return s.pid == 100; });
    ASSERT_NE(it1, snaps.end());
    EXPECT_EQ(it1->gpuMemoryBytes, 256ULL * 1024 * 1024);

    // Find process 200
    auto it2 = std::find_if(snaps.begin(), snaps.end(), [](const auto& s) { return s.pid == 200; });
    ASSERT_NE(it2, snaps.end());
    EXPECT_EQ(it2->gpuMemoryBytes, 128ULL * 1024 * 1024);
}

TEST(ProcessModelTest, MergeGPUDataAggregatesMultiGPU)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "multi_gpu_proc", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "GPU 0", "Vendor")
        .withGPU("GPU1", "GPU 1", "Vendor")
        .withProcessGPU(100, "GPU0", 256ULL * 1024 * 1024)
        .withProcessGPU(100, "GPU1", 512ULL * 1024 * 1024);

    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);

    processModel.refresh(); // Seed: the processes are listed before the GPU read, as in the app (#1417)
    gpuModel->refresh();
    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Memory should be summed across GPUs
    EXPECT_EQ(snaps[0].gpuMemoryBytes, (256 + 512) * 1024ULL * 1024);
}

namespace
{

Platform::ProcessGPUCounters
makeProcessGpuUsage(std::int32_t pid, const std::string& gpuId, double utilPercent, std::uint64_t dedicatedBytes, std::uint64_t sharedBytes)
{
    Platform::ProcessGPUCounters counters;
    counters.pid = pid;
    counters.gpuId = gpuId;
    counters.gpuUtilPercent = utilPercent;
    counters.gpuMemoryBytes = dedicatedBytes;
    counters.gpuSharedMemoryBytes = sharedBytes;
    counters.activeEngines = {"3D"};
    return counters;
}

/// One refresh of a process model sharing a GPU model built from `gpuProbe`; the snapshots.
std::vector<Domain::ProcessSnapshot> mergeOnce(std::unique_ptr<MockGPUProbe> gpuProbe)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "gpu_proc", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);

    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);
    processModel.refresh(); // Seed: the processes are listed before the GPU read, as in the app (#1417)
    gpuModel->refresh();
    processModel.refresh();
    return processModel.snapshots();
}

} // namespace

// #1164: a process's GPU utilization is the busiest GPU's, 0-100 like an adapter's, not a sum across
// GPUs (which passed 100% in the table while Process Details clamped it). Encoder/decoder likewise.
TEST(ProcessModelTest, MergeGPUDataTakesTheBusiestGpusUtilization)
{
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    auto onGpu0 = makeProcessGpuUsage(100, "GPU0", 60.0, 0, 0);
    onGpu0.encoderUtilPercent = 30.0;
    auto onGpu1 = makeProcessGpuUsage(100, "GPU1", 70.0, 0, 0);
    onGpu1.decoderUtilPercent = 20.0;
    gpuProbe->withGPU("GPU0", "GPU 0").withGPU("GPU1", "GPU 1").withProcessGPUCounters(onGpu0).withProcessGPUCounters(onGpu1);

    const auto snaps = mergeOnce(std::move(gpuProbe));
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_DOUBLE_EQ(snaps[0].gpuUtilPercent, 70.0);
    EXPECT_DOUBLE_EQ(snaps[0].gpuEncoderUtil, 30.0);
    EXPECT_DOUBLE_EQ(snaps[0].gpuDecoderUtil, 20.0);
    ASSERT_EQ(snaps[0].perGpuUsage.size(), 2U);
    EXPECT_DOUBLE_EQ(snaps[0].perGpuUsage[0].utilPercent, 60.0);
    EXPECT_DOUBLE_EQ(snaps[0].perGpuUsage[1].utilPercent, 70.0);
}

// #1164: a per-GPU reading past 100% (or NaN) is clamped like an adapter's, so the table and Process
// Details agree.
TEST(ProcessModelTest, MergeGPUDataClampsUtilizationToAPercentage)
{
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    gpuProbe->withGPU("GPU0", "GPU 0")
        .withGPU("GPU1", "GPU 1")
        .withProcessGPUCounters(makeProcessGpuUsage(100, "GPU0", 130.0, 0, 0))
        .withProcessGPUCounters(makeProcessGpuUsage(100, "GPU1", std::numeric_limits<double>::quiet_NaN(), 0, 0));

    const auto snaps = mergeOnce(std::move(gpuProbe));
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_DOUBLE_EQ(snaps[0].gpuUtilPercent, 100.0);
    ASSERT_EQ(snaps[0].perGpuUsage.size(), 2U);
    EXPECT_DOUBLE_EQ(snaps[0].perGpuUsage[0].utilPercent, 100.0);
    EXPECT_DOUBLE_EQ(snaps[0].perGpuUsage[1].utilPercent, 0.0);
}

// #1164: dedicated and shared GPU memory are kept apart, and "GPU memory" counts on each GPU what
// that GPU's used figure on the GPU tab counts -- dedicated on a discrete GPU, shared on a Windows
// integrated one -- so it can't exceed what the adapters show in use (a game's shared memory on a
// dGPU used to be added to its VRAM).
TEST(ProcessModelTest, MergeGPUDataKeepsDedicatedAndSharedMemoryApart)
{
    constexpr std::uint64_t MIB = 1024ULL * 1024ULL;
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    gpuProbe->withGPU("GPU0", "Discrete GPU", "Vendor", false)
        .withSharedMemoryGPU("GPU1", "Integrated GPU", "Vendor")
        .withProcessGPUCounters(makeProcessGpuUsage(100, "GPU0", 50.0, 300 * MIB, 200 * MIB))
        .withProcessGPUCounters(makeProcessGpuUsage(100, "GPU1", 10.0, 10 * MIB, 400 * MIB));

    const auto snaps = mergeOnce(std::move(gpuProbe));
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_EQ(snaps[0].gpuMemoryBytes, (300 + 400) * MIB); // the discrete GPU's VRAM, the integrated GPU's shared
    EXPECT_EQ(snaps[0].gpuDedicatedMemoryBytes, (300 + 10) * MIB);
    EXPECT_EQ(snaps[0].gpuSharedMemoryBytes, (200 + 400) * MIB);
    ASSERT_EQ(snaps[0].perGpuUsage.size(), 2U);
    EXPECT_EQ(snaps[0].perGpuUsage[0].memoryBytes, 300 * MIB);
    EXPECT_EQ(snaps[0].perGpuUsage[0].dedicatedMemoryBytes, 300 * MIB);
    EXPECT_EQ(snaps[0].perGpuUsage[0].sharedMemoryBytes, 200 * MIB);
    EXPECT_EQ(snaps[0].perGpuUsage[1].memoryBytes, 400 * MIB);
    EXPECT_EQ(snaps[0].perGpuUsage[1].dedicatedMemoryBytes, 10 * MIB);
    EXPECT_EQ(snaps[0].perGpuUsage[1].sharedMemoryBytes, 400 * MIB);
}

// #1164: a platform with no shared segment (NVML, ROCm SMI) reports dedicated memory only, which is
// then the GPU memory figure on a discrete GPU, as before.
TEST(ProcessModelTest, MergeGPUDataWithDedicatedMemoryOnly)
{
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    gpuProbe->withGPU("GPU0", "Discrete GPU").withProcessGPUCounters(makeProcessGpuUsage(100, "GPU0", 40.0, 4096, 0));

    const auto snaps = mergeOnce(std::move(gpuProbe));
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_EQ(snaps[0].gpuMemoryBytes, 4096U);
    EXPECT_EQ(snaps[0].gpuDedicatedMemoryBytes, 4096U);
    EXPECT_EQ(snaps[0].gpuSharedMemoryBytes, 0U);
}

// #1164: an integrated GPU on a platform with no shared segment (an AMD APU under ROCm SMI, whose
// used figure is its VRAM carve-out) counts its dedicated memory, not a shared 0.
TEST(ProcessModelTest, MergeGPUDataIntegratedGpuWithoutSharedMemoryCountsDedicated)
{
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    gpuProbe->withGPU("GPU0", "APU", "AMD", true).withProcessGPUCounters(makeProcessGpuUsage(100, "GPU0", 40.0, 2048, 0));

    const auto snaps = mergeOnce(std::move(gpuProbe));
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_EQ(snaps[0].gpuMemoryBytes, 2048U);
    ASSERT_EQ(snaps[0].perGpuUsage.size(), 1U);
    EXPECT_EQ(snaps[0].perGpuUsage[0].memoryBytes, 2048U);
}

// #1164: which segment counts is the adapter's (GPUInfo::memoryIsShared), not inferred from the
// reading: a Windows integrated GPU's process with dedicated bytes but 0 shared bytes shows a
// shared 0, as the adapter's used figure is its shared segment, 0 included -- "GPU memory" doesn't
// switch to dedicated and back as shared usage crosses 0.
TEST(ProcessModelTest, MergeGPUDataSharedSegmentGpuWithZeroSharedUsageCountsShared)
{
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    gpuProbe->withSharedMemoryGPU("GPU0", "Integrated GPU", "Intel")
        .withProcessGPUCounters(makeProcessGpuUsage(100, "GPU0", 25.0, 4096, 0));

    const auto snaps = mergeOnce(std::move(gpuProbe));
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_EQ(snaps[0].gpuMemoryBytes, 0U);
    EXPECT_EQ(snaps[0].gpuDedicatedMemoryBytes, 4096U);
    EXPECT_EQ(snaps[0].gpuSharedMemoryBytes, 0U);
    ASSERT_EQ(snaps[0].perGpuUsage.size(), 1U);
    EXPECT_EQ(snaps[0].perGpuUsage[0].memoryBytes, 0U);
    EXPECT_EQ(snaps[0].perGpuUsage[0].dedicatedMemoryBytes, 4096U);
}

// #1164: and an integrated GPU whose figures count dedicated memory (Linux) counts dedicated even
// with a shared amount reported beside it.
TEST(ProcessModelTest, MergeGPUDataDedicatedSegmentIntegratedGpuIgnoresSharedReading)
{
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    gpuProbe->withGPU("GPU0", "APU", "AMD", true).withProcessGPUCounters(makeProcessGpuUsage(100, "GPU0", 40.0, 2048, 512));

    const auto snaps = mergeOnce(std::move(gpuProbe));
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_EQ(snaps[0].gpuMemoryBytes, 2048U);
    EXPECT_EQ(snaps[0].gpuSharedMemoryBytes, 512U);
}

// #1799: a process's samples on the NPU (the probe tags them with its kind, PDH naming the NPU by
// its LUID) are its NPU figures, never counted in its GPU ones: a process that only uses the NPU
// has no GPU figures and no GPU device, and one using both has the iGPU's GPU figures alone.
TEST(ProcessModelTest, MergeGPUDataRoutesNpuSamplesToTheNpuFields)
{
    constexpr std::uint64_t MIB = 1024ULL * 1024ULL;
    const std::string npuLuid = "GPU_0x00000000_0x00018103";
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "npu_only", 'R', 1000, 500), makeCounter(200, "gpu_and_npu", 'R', 2000, 1000)});
    processProbe->setTotalCpuTime(100000);

    const auto onNpu = [&npuLuid](std::int32_t pid, double util, std::uint64_t sharedBytes)
    {
        auto counters = makeProcessGpuUsage(pid, npuLuid, util, 0, sharedBytes);
        counters.kind = Platform::DeviceKind::Npu;
        counters.activeEngines = {"Compute"};
        return counters;
    };
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    gpuProbe->withSharedMemoryGPU("iGPU", "Intel(R) Arc(TM) 140T GPU", "Intel")
        .withAccelerator("NPU", "Intel(R) AI Boost", npuLuid)
        .withProcessGPUCounters(onNpu(100, 60.0, 8 * MIB))
        .withProcessGPUCounters(makeProcessGpuUsage(200, "iGPU", 20.0, 0, 300 * MIB))
        .withProcessGPUCounters(onNpu(200, 80.0, 16 * MIB));
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    caps.hasPerProcessUtilization = true;
    gpuProbe->withCapabilities(caps);

    const auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);
    processModel.refresh(); // Seed: the processes are listed before the GPU read, as in the app (#1417)
    gpuModel->refresh();
    processModel.refresh();

    const auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 2U);
    const auto byPid = [&snaps](std::int32_t pid) -> const Domain::ProcessSnapshot&
    {
        return *std::ranges::find(snaps, pid, &Domain::ProcessSnapshot::pid);
    };

    const Domain::ProcessSnapshot& npuOnly = byPid(100);
    EXPECT_TRUE(npuOnly.gpuFieldsRead);
    EXPECT_DOUBLE_EQ(npuOnly.gpuUtilPercent, 0.0) << "The NPU's 60% isn't GPU work";
    EXPECT_EQ(npuOnly.gpuMemoryBytes, 0U);
    EXPECT_EQ(npuOnly.gpuSharedMemoryBytes, 0U);
    EXPECT_TRUE(npuOnly.gpuDevices.empty()) << "No GPU device, let alone a raw GPU_0x... id";
    EXPECT_TRUE(npuOnly.perGpuUsage.empty());
    EXPECT_TRUE(npuOnly.gpuEngines.empty());
    EXPECT_DOUBLE_EQ(npuOnly.npuUtilPercent, 60.0);
    EXPECT_EQ(npuOnly.npuMemoryBytes, 8 * MIB) << "The NPU's figures count its shared segment";
    EXPECT_EQ(npuOnly.npuSharedMemoryBytes, 8 * MIB);
    EXPECT_EQ(npuOnly.npuDedicatedMemoryBytes, 0U);
    EXPECT_EQ(npuOnly.npuEngines, std::vector<std::string>{"Compute"});
    EXPECT_EQ(npuOnly.npuDevices, "Intel(R) AI Boost");

    const Domain::ProcessSnapshot& both = byPid(200);
    EXPECT_DOUBLE_EQ(both.gpuUtilPercent, 20.0) << "The iGPU's own figure, not the NPU's 80%";
    EXPECT_EQ(both.gpuMemoryBytes, 300 * MIB);
    EXPECT_EQ(both.gpuDevices, "Intel(R) Arc(TM) 140T GPU");
    EXPECT_EQ(both.gpuEngines, std::vector<std::string>{"3D"});
    ASSERT_EQ(both.perGpuUsage.size(), 1U);
    EXPECT_DOUBLE_EQ(both.npuUtilPercent, 80.0);
    EXPECT_EQ(both.npuMemoryBytes, 16 * MIB);
    EXPECT_EQ(both.npuDevices, "Intel(R) AI Boost");
}

// #1869: the watched process's samples, which Process Details' history is built from, carry its NPU
// figures with every generation, as they do its GPU figures.
TEST(ProcessModelTest, WatchedSamplesCarryTheNpuFigures)
{
    const std::string npuLuid = "GPU_0x00000000_0x00018103";
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "npu_user", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);
    auto counters = makeProcessGpuUsage(100, npuLuid, 35.0, 2048, 4096);
    counters.kind = Platform::DeviceKind::Npu;
    counters.activeEngines = {"Compute"};
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    gpuProbe->withAccelerator("NPU", "Intel(R) AI Boost", npuLuid).withProcessGPUCounters(counters);
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    caps.hasPerProcessUtilization = true;
    gpuProbe->withCapabilities(caps);

    const auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);
    processModel.watchProcess(100);
    processModel.refresh(); // Listed before the GPU read, as in the app (#1417): its GPU fields unread
    gpuModel->refresh();
    processModel.refresh();

    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(processModel.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 2U);
    ASSERT_NE(samples[0].snapshot, nullptr);
    EXPECT_FALSE(samples[0].snapshot->gpuFieldsRead);
    EXPECT_DOUBLE_EQ(samples[0].snapshot->npuUtilPercent, 0.0);
    ASSERT_NE(samples[1].snapshot, nullptr);
    const Domain::ProcessSnapshot& watched = *samples[1].snapshot;
    EXPECT_TRUE(watched.gpuFieldsRead);
    EXPECT_TRUE(samples[1].gpuPerProcessSupported);
    EXPECT_TRUE(samples[1].gpuUtilizationSupported);
    EXPECT_DOUBLE_EQ(watched.npuUtilPercent, 35.0);
    EXPECT_EQ(watched.npuDedicatedMemoryBytes, 2048U);
    EXPECT_EQ(watched.npuSharedMemoryBytes, 4096U);
    EXPECT_EQ(watched.npuEngines, std::vector<std::string>{"Compute"});
    EXPECT_EQ(watched.npuDevices, "Intel(R) AI Boost");
    EXPECT_DOUBLE_EQ(watched.gpuUtilPercent, 0.0) << "Never counted as GPU use";
}

// #3789: a read that lists a PID twice (reused within one read): Process Details' watched sample is the same row the
// table's findSnapshot() shows, the last listing, as the generation's PID index maps it, not the first one a scan meets.
TEST(ProcessModelTest, TheWatchedSampleOfAPidListedTwiceIsTheRowTheTableShows)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(makeProcessCounters(100, "first", 'S', 100, 50, 10, 1024, 1))
        .withProcess(makeProcessCounters(100, "second", 'S', 100, 50, 20, 1024, 1));
    Domain::ProcessModel processModel(std::move(probe));
    processModel.watchProcess(100);
    processModel.refresh();

    const std::shared_ptr<const Domain::ProcessSnapshot> shown = processModel.findSnapshot(100);
    ASSERT_NE(shown, nullptr);
    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(processModel.watchedSamplesSince(0, samples));
    ASSERT_FALSE(samples.empty());
    ASSERT_NE(samples.back().snapshot, nullptr);
    EXPECT_EQ(samples.back().snapshot->name, shown->name);
    EXPECT_EQ(samples.back().snapshot->name, "second");
}

// #1799: a sample on an adapter enumerated as an NPU is routed by the adapter's kind too, even
// untagged, so a probe that doesn't tag its samples still never counts NPU work as GPU work.
TEST(ProcessModelTest, MergeGPUDataRoutesAnUntaggedSampleOnAnEnumeratedNpuToTheNpuFields)
{
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    gpuProbe->withAccelerator("NPU", "Intel(R) AI Boost", "GPU_0x00000000_0x00018103")
        .withProcessGPUCounters(makeProcessGpuUsage(100, "GPU_0x00000000_0x00018103", 45.0, 0, 4096));

    const auto snaps = mergeOnce(std::move(gpuProbe));
    ASSERT_EQ(snaps.size(), 1U);
    EXPECT_DOUBLE_EQ(snaps[0].gpuUtilPercent, 0.0);
    EXPECT_TRUE(snaps[0].gpuDevices.empty());
    EXPECT_DOUBLE_EQ(snaps[0].npuUtilPercent, 45.0);
    EXPECT_EQ(snaps[0].npuMemoryBytes, 4096U);
}

TEST(ProcessModelTest, MergeGPUDataWithNoGPUModel)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "no_gpu_proc", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);

    Domain::ProcessModel processModel(std::move(processProbe));
    // No GPU model set

    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].gpuMemoryBytes, 0); // No GPU data
}

TEST(ProcessModelTest, MergeGPUDataUpdatesGpuDevices)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "gpu_proc", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "NVIDIA RTX 3080", "NVIDIA").withProcessGPU(100, "GPU0", 1ULL * 1024 * 1024 * 1024);

    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);

    processModel.refresh(); // Seed: the processes are listed before the GPU read, as in the app (#1417)
    gpuModel->refresh();
    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // gpuDevices should contain the GPU name
    EXPECT_FALSE(snaps[0].gpuDevices.empty());
}

// #1210: tryCopySnapshotsIfNewer() hands out the GPU support published with the generation.
TEST(ProcessModelTest, CopiedSnapshotsComeWithTheirGenerationsGpuSupport)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    caps.hasPerProcessUtilization = false; // NVML-like
    gpuProbe->withCapabilities(caps);
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));

    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);
    gpuModel->refresh();
    processModel.refresh();

    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> snapshots;
    std::uint64_t version = 0;
    Domain::ProcessModel::GpuSupport support{.perProcess = false, .utilization = true};
    ASSERT_TRUE(processModel.tryCopySnapshotsIfNewer(0, snapshots, version, nullptr, &support));
    EXPECT_TRUE(support.perProcess);
    EXPECT_FALSE(support.utilization);
}

// =============================================================================
// One GPU-acquisition owner (#1417): the GPU sampler reads the per-process GPU
// counters and publishes them; the process sampler merges the newest
// publication and never calls the probe.
// =============================================================================

namespace
{

constexpr std::uint64_t GPU_PROCESS_MEMORY = 512ULL * 1024 * 1024;

/// A GPU probe reporting pid 100 on GPU0, with per-process metrics and
/// utilization supported.
std::unique_ptr<MockGPUProbe> makePerProcessGpuProbe()
{
    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    caps.hasPerProcessUtilization = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Test GPU", "TestVendor").withProcessGPU(100, "GPU0", GPU_PROCESS_MEMORY);
    return gpuProbe;
}

/// A process probe listing pid 100.
std::unique_ptr<MockProcessProbe> makeGpuProcessProbe()
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);
    return processProbe;
}

/// The snapshot of @p pid in @p snapshots; fails the test when it is missing.
const Domain::ProcessSnapshot* findPid(const std::vector<Domain::ProcessSnapshot>& snapshots, std::int32_t pid)
{
    const auto it = std::ranges::find(snapshots, pid, &Domain::ProcessSnapshot::pid);
    EXPECT_NE(it, snapshots.end()) << "pid " << pid;
    return it != snapshots.end() ? &*it : nullptr;
}

/// The GPU support published with @p model's latest generation.
Domain::ProcessModel::GpuSupport latestGpuSupport(const Domain::ProcessModel& model)
{
    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> snapshots;
    std::uint64_t version = 0;
    Domain::ProcessModel::GpuSupport support;
    EXPECT_TRUE(model.tryCopySnapshotsIfNewer(0, snapshots, version, nullptr, &support));
    return support;
}

} // namespace

// The process sampler merges the GPU sampler's publication: the probe's
// per-process read runs only on the thread that refreshes the GPU model, once
// per GPU refresh, however often processes refresh.
TEST(ProcessModelTest, ProcessRefreshNeverCallsTheGpuProbe)
{
    auto currentTime = Domain::ProcessModel::Clock::time_point{} + std::chrono::hours(1);
    auto gpuProbe = makePerProcessGpuProbe();
    auto* rawGpuProbe = gpuProbe.get();
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(makeGpuProcessProbe(), [&currentTime] { return currentTime; });
    processModel.setGPUModel(gpuModel);
    processModel.refresh(); // Seed: listed before the GPU read, as in the app

    std::thread gpuSampler([&gpuModel, currentTime] { gpuModel->refreshAt(currentTime); });
    const auto gpuSamplerId = gpuSampler.get_id();
    gpuSampler.join();
    ASSERT_EQ(rawGpuProbe->readProcessCountersCallCount(), 1U);

    for (int i = 0; i < 5; ++i)
    {
        currentTime += std::chrono::milliseconds(500);
        processModel.refresh();
    }

    EXPECT_EQ(rawGpuProbe->readProcessCountersCallCount(), 1U) << "a process refresh ran the GPU probe";
    const auto threads = rawGpuProbe->processReadThreads();
    ASSERT_EQ(threads.size(), 1U);
    EXPECT_EQ(*threads.begin(), gpuSamplerId);
    EXPECT_FALSE(threads.contains(std::this_thread::get_id()));

    // Per-process values still flow into the process snapshots.
    const auto snapshots = processModel.snapshots();
    ASSERT_EQ(snapshots.size(), 1U);
    EXPECT_TRUE(snapshots[0].gpuFieldsRead);
    EXPECT_EQ(snapshots[0].gpuMemoryBytes, GPU_PROCESS_MEMORY);
    EXPECT_DOUBLE_EQ(snapshots[0].gpuUtilPercent, 25.0);
    EXPECT_EQ(snapshots[0].gpuDevices, "Test GPU");
    ASSERT_EQ(snapshots[0].perGpuUsage.size(), 1U);
    EXPECT_EQ(snapshots[0].perGpuUsage[0].gpuName, "Test GPU");
}

// A GPU read holding the probe lock (a slow or hung driver query) used to block
// the process sampler, which took the same lock for its own per-process read.
// It no longer waits.
TEST(ProcessModelTest, ProcessRefreshDoesNotWaitForABlockedGpuProbe)
{
    auto gpuProbe = makePerProcessGpuProbe();
    auto* rawGpuProbe = gpuProbe.get();
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(makeGpuProcessProbe());
    processModel.setGPUModel(gpuModel);
    processModel.refresh(); // Seed: listed before the GPU read
    gpuModel->refresh();

    rawGpuProbe->armBlockingReadGPUCounters();
    std::thread gpuSampler([&gpuModel] { gpuModel->refresh(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!rawGpuProbe->hasEnteredBlockedReadGPUCounters() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(rawGpuProbe->hasEnteredBlockedReadGPUCounters()) << "the GPU refresh never reached the probe";

    auto processSample = std::async(std::launch::async, [&processModel] { processModel.refresh(); });
    const auto status = processSample.wait_for(std::chrono::seconds(5));
    EXPECT_EQ(status, std::future_status::ready) << "the process refresh waited for the GPU probe";

    rawGpuProbe->releaseBlockedReadGPUCounters();
    gpuSampler.join();
    processSample.wait();

    const auto snapshots = processModel.snapshots();
    ASSERT_EQ(snapshots.size(), 1U);
    EXPECT_EQ(snapshots[0].gpuMemoryBytes, GPU_PROCESS_MEMORY) << "merged from the publication made before the block";
}

// Evidence for #1417 (not a gate): with a deliberately slow per-process GPU read (a slow driver)
// and the GPU sampler reading in the background, per-process values still flow, and the slowest
// process refresh is recorded as a test property. Before, every process refresh made that read
// itself, so each took at least SLOW_READ.
TEST(ProcessModelTest, ProcessSamplingDoesNotWaitForASlowGpuProbe)
{
    constexpr auto SLOW_READ = std::chrono::milliseconds(400);
    auto gpuProbe = makePerProcessGpuProbe();
    gpuProbe->withProcessReadDelay(SLOW_READ);
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(makeGpuProcessProbe());
    processModel.setGPUModel(gpuModel);
    processModel.refresh(); // Seed: listed before the GPU read
    gpuModel->refresh();    // One slow read, so there is a publication to merge

    std::atomic<bool> stop{false};
    std::thread gpuSampler(
        [&gpuModel, &stop]
        {
            while (!stop.load(std::memory_order_acquire))
            {
                gpuModel->refresh();
            }
        });

    auto slowest = std::chrono::steady_clock::duration::zero();
    for (int i = 0; i < 5; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        processModel.refresh();
        slowest = std::max(slowest, std::chrono::steady_clock::now() - start);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    stop.store(true, std::memory_order_release);
    gpuSampler.join();

    const auto slowestUs = std::chrono::duration_cast<std::chrono::microseconds>(slowest).count();
    RecordProperty("slowest_process_refresh_us", std::to_string(slowestUs));
    RecordProperty("slow_gpu_read_us", std::to_string(std::chrono::microseconds(SLOW_READ).count()));
    // Recorded, not asserted: a wall-clock bound measures scheduling, and a preempted CI worker
    // would fail it although the process sampler never waits. The ownership boundary itself is
    // proven deterministically by ProcessRefreshDoesNotWaitForABlockedGpuProbe.

    const auto snapshots = processModel.snapshots();
    ASSERT_EQ(snapshots.size(), 1U);
    EXPECT_EQ(snapshots[0].gpuMemoryBytes, GPU_PROCESS_MEMORY);
}

// #1210: a generation is stamped with the support its GPU fields were read
// under, which comes with the publication they came from; the watched samples
// keep each generation's own.
TEST(ProcessModelTest, GpuSupportComesWithThePublicationItWasReadUnder)
{
    auto currentTime = Domain::ProcessModel::Clock::time_point{} + std::chrono::hours(1);
    auto gpuProbe = makePerProcessGpuProbe();
    auto* rawGpuProbe = gpuProbe.get();
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(makeGpuProcessProbe(), [&currentTime] { return currentTime; });
    processModel.setGPUModel(gpuModel);
    processModel.watchProcess(100);

    gpuModel->refreshAt(currentTime);
    processModel.refresh(); // Read with utilization supported

    // The probe loses per-process utilization; the next GPU refresh publishes
    // under that.
    Platform::GPUCapabilities withoutUtilization;
    withoutUtilization.hasPerProcessMetrics = true;
    rawGpuProbe->withCapabilities(withoutUtilization).withRescanReportingChange();
    currentTime += std::chrono::milliseconds(500);
    gpuModel->refreshAt(currentTime);
    ASSERT_TRUE(gpuModel->perProcessUtilizationKnownUnsupported());
    currentTime += std::chrono::milliseconds(500);
    processModel.refresh();

    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(processModel.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 2U);
    EXPECT_TRUE(samples[0].gpuUtilizationSupported);
    EXPECT_FALSE(samples[1].gpuUtilizationSupported);
    EXPECT_TRUE(samples[1].gpuPerProcessSupported);
    ASSERT_NE(samples[1].snapshot, nullptr);
    EXPECT_EQ(samples[1].snapshot->gpuMemoryBytes, GPU_PROCESS_MEMORY);

    const auto support = latestGpuSupport(processModel);
    EXPECT_TRUE(support.perProcess);
    EXPECT_FALSE(support.utilization);
    EXPECT_FALSE(support.readFailed);
}

// #1210, #1417: a process first listed after the GPU sampler's last read has GPU
// fields that read could not have seen: unread, not measured zeros, even if the
// read has an entry for its pid. A process listed before the read keeps its
// figures, or is a measured zero if the read has none for it. Before the first
// read, nothing is read.
TEST(ProcessModelTest, AProcessFirstListedSinceTheGpuReadHasUnreadGpuFields)
{
    const auto start = Domain::ProcessModel::Clock::time_point{} + std::chrono::hours(1);
    auto currentTime = start;
    auto processProbe = std::make_unique<MockProcessProbe>();
    auto* rawProcessProbe = processProbe.get();
    rawProcessProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1000, 500), makeCounter(150, "idle_process", 'S', 10, 5, 6000)});
    rawProcessProbe->setTotalCpuTime(100000);
    auto gpuProbe = makePerProcessGpuProbe();
    gpuProbe->withProcessGPU(300, "GPU0", 1024); // Uses the GPU, but listed only after the read
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe), [&currentTime] { return currentTime; });
    processModel.setGPUModel(gpuModel);

    processModel.refresh(); // No GPU read yet
    for (const auto& snap : processModel.snapshots())
    {
        EXPECT_FALSE(snap.gpuFieldsRead) << "pid " << snap.pid << ": nothing read before the first GPU refresh";
    }
    EXPECT_TRUE(latestGpuSupport(processModel).perProcess);

    gpuModel->refreshAt(start + std::chrono::seconds(1));
    currentTime = start + std::chrono::milliseconds(1500);
    rawProcessProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1100, 500),
                                  makeCounter(150, "idle_process", 'S', 10, 5, 6000),
                                  makeCounter(200, "new_process", 'R', 10, 5, 7000),
                                  makeCounter(300, "new_gpu_process", 'R', 10, 5, 7100)});
    rawProcessProbe->setTotalCpuTime(200000);
    processModel.refresh();

    auto snapshots = processModel.snapshots();
    ASSERT_EQ(snapshots.size(), 4U);
    if (const auto* gpuUser = findPid(snapshots, 100); gpuUser != nullptr)
    {
        EXPECT_TRUE(gpuUser->gpuFieldsRead);
        EXPECT_EQ(gpuUser->gpuMemoryBytes, GPU_PROCESS_MEMORY);
    }
    if (const auto* idle = findPid(snapshots, 150); idle != nullptr)
    {
        EXPECT_TRUE(idle->gpuFieldsRead) << "listed before the read: a measured zero";
        EXPECT_EQ(idle->gpuMemoryBytes, 0U);
    }
    if (const auto* newcomer = findPid(snapshots, 200); newcomer != nullptr)
    {
        EXPECT_FALSE(newcomer->gpuFieldsRead) << "first listed since the GPU read";
    }
    if (const auto* newGpuUser = findPid(snapshots, 300); newGpuUser != nullptr)
    {
        // The read's entry for pid 300 can't be this process's: it was not yet listed.
        EXPECT_FALSE(newGpuUser->gpuFieldsRead) << "first listed since the GPU read";
        EXPECT_EQ(newGpuUser->gpuMemoryBytes, 0U);
    }

    // The next GPU read covers everyone.
    gpuModel->refreshAt(start + std::chrono::seconds(2));
    currentTime = start + std::chrono::milliseconds(2500);
    processModel.refresh();
    for (const auto& snap : processModel.snapshots())
    {
        EXPECT_TRUE(snap.gpuFieldsRead) << "pid " << snap.pid;
    }
    snapshots = processModel.snapshots();
    if (const auto* gpuUser = findPid(snapshots, 300); gpuUser != nullptr)
    {
        EXPECT_EQ(gpuUser->gpuMemoryBytes, 1024U) << "listed before this read, so its entry merges";
    }
}

// #1417: a GPU read taken while an earlier process held a pid publishes that process's usage under
// the pid. A process that reuses the pid and is first listed after the read must not inherit it:
// its GPU fields are unread and empty. The synchronous read this replaced could not mix them up.
TEST(ProcessModelTest, APidReusedSinceTheGpuReadDoesNotInheritTheExitedProcessGpuUsage)
{
    const auto start = Domain::ProcessModel::Clock::time_point{} + std::chrono::hours(1);
    auto currentTime = start;
    auto processProbe = std::make_unique<MockProcessProbe>();
    auto* rawProcessProbe = processProbe.get();
    rawProcessProbe->setCounters({makeCounter(42, "old_owner", 'R', 1000, 500, 5000)});
    rawProcessProbe->setTotalCpuTime(100000);
    auto gpuProbe = makePerProcessGpuProbe();
    gpuProbe->withProcessGPU(42, "GPU0", 4096);
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe), [&currentTime] { return currentTime; });
    processModel.setGPUModel(gpuModel);

    // Pid 42 (first owner) is listed before the read at T: its usage merges.
    processModel.refresh();
    const auto captureTime = start + std::chrono::seconds(1);
    gpuModel->refreshAt(captureTime);
    currentTime = start + std::chrono::milliseconds(1500);
    processModel.refresh();
    {
        const auto snapshots = processModel.snapshots();
        ASSERT_EQ(snapshots.size(), 1U);
        EXPECT_TRUE(snapshots[0].gpuFieldsRead);
        EXPECT_EQ(snapshots[0].gpuMemoryBytes, 4096U);
    }

    // The first owner exits and a new process takes pid 42 (a different start time), first listed
    // after T. The publication still holds the old owner's entry for pid 42.
    currentTime = start + std::chrono::seconds(2);
    rawProcessProbe->setCounters({makeCounter(42, "new_owner", 'R', 10, 5, 9000)});
    rawProcessProbe->setTotalCpuTime(200000);
    processModel.refresh();
    {
        const auto snapshots = processModel.snapshots();
        ASSERT_EQ(snapshots.size(), 1U);
        EXPECT_EQ(snapshots[0].name, "new_owner");
        EXPECT_FALSE(snapshots[0].gpuFieldsRead);
        EXPECT_EQ(snapshots[0].gpuMemoryBytes, 0U);
        EXPECT_DOUBLE_EQ(snapshots[0].gpuUtilPercent, 0.0);
        EXPECT_TRUE(snapshots[0].perGpuUsage.empty());
        EXPECT_TRUE(snapshots[0].gpuDevices.empty());
    }
}

// #1417: the process sampler merges the newest publication up to
// PROCESS_GPU_DATA_MAX_AGE_MS old. An older one means the GPU sampler has
// stalled: the generation has no GPU fields and is marked a failed read, rather
// than showing long-gone figures as current. A fresh publication restores them.
TEST(ProcessModelTest, StalePerProcessGpuDataIsAGapNotAReading)
{
    const auto start = Domain::ProcessModel::Clock::time_point{} + std::chrono::hours(1);
    const auto maxAge = std::chrono::milliseconds(Domain::Sampling::PROCESS_GPU_DATA_MAX_AGE_MS);
    auto currentTime = start;
    auto gpuModel = std::make_shared<Domain::GPUModel>(makePerProcessGpuProbe());
    Domain::ProcessModel processModel(makeGpuProcessProbe(), [&currentTime] { return currentTime; });
    processModel.setGPUModel(gpuModel);
    processModel.watchProcess(100);

    processModel.refresh(); // Seed at start: listed before the GPU read
    gpuModel->refreshAt(start);
    currentTime = start + maxAge; // As old as it may be: still merged
    processModel.refresh();
    {
        const auto support = latestGpuSupport(processModel);
        EXPECT_FALSE(support.readFailed);
        EXPECT_EQ(processModel.snapshots().at(0).gpuMemoryBytes, GPU_PROCESS_MEMORY);
    }

    currentTime = start + maxAge + std::chrono::milliseconds(1); // Too old
    processModel.refresh();
    {
        const auto support = latestGpuSupport(processModel);
        EXPECT_TRUE(support.perProcess) << "stale is not unsupported";
        EXPECT_TRUE(support.readFailed);
        EXPECT_EQ(processModel.snapshots().at(0).gpuMemoryBytes, 0U);
        std::vector<Domain::ProcessSample> samples;
        ASSERT_TRUE(processModel.watchedSamplesSince(0, samples));
        ASSERT_FALSE(samples.empty());
        EXPECT_TRUE(samples.back().gpuReadFailed) << "Process Details shows a gap";
    }

    // The GPU sampler catches up.
    gpuModel->refreshAt(currentTime);
    currentTime += std::chrono::milliseconds(500);
    processModel.refresh();
    EXPECT_FALSE(latestGpuSupport(processModel).readFailed);
    EXPECT_EQ(processModel.snapshots().at(0).gpuMemoryBytes, GPU_PROCESS_MEMORY);
}

// #1142, #1210: a per-process read that throws is published by the GPU sampler
// as a failed read, so the process generations are stamped with the support it
// ran under and marked failed.
TEST(ProcessModelTest, AFailedGpuReadIsPublishedAsAFailedRead)
{
    auto gpuProbe = makePerProcessGpuProbe();
    gpuProbe->withProcessCountersThrowing();
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    gpuModel->refresh();
    Domain::ProcessModel processModel(makeGpuProcessProbe());
    processModel.setGPUModel(gpuModel);
    processModel.refresh();

    const auto support = latestGpuSupport(processModel);
    EXPECT_TRUE(support.perProcess);
    EXPECT_TRUE(support.utilization);
    EXPECT_TRUE(support.readFailed);
    EXPECT_EQ(processModel.snapshots().at(0).gpuMemoryBytes, 0U);
}

// For TSan (#1417): the GPU sampler publishing, the process sampler merging and
// the UI reading, all at once, with no lock shared between the two samplers.
TEST(ProcessModelTest, GpuAndProcessSamplersRunConcurrentlyWithoutRaces)
{
    auto gpuModel = std::make_shared<Domain::GPUModel>(makePerProcessGpuProbe());
    Domain::ProcessModel processModel(makeGpuProcessProbe());
    processModel.setGPUModel(gpuModel);

    constexpr int ITERATIONS = 200;
    std::atomic<bool> done{false};
    std::thread gpuSampler(
        [&gpuModel]
        {
            for (int i = 0; i < ITERATIONS; ++i)
            {
                gpuModel->refresh();
            }
        });
    std::thread processSampler(
        [&processModel]
        {
            for (int i = 0; i < ITERATIONS; ++i)
            {
                processModel.refresh();
            }
        });
    std::thread reader(
        [&processModel, &gpuModel, &done]
        {
            std::uint64_t seen = 0;
            std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> snapshots;
            Domain::ProcessModel::GpuSupport support;
            while (!done.load(std::memory_order_acquire))
            {
                static_cast<void>(processModel.tryCopySnapshotsIfNewer(seen, snapshots, seen, nullptr, &support));
                const auto publication = gpuModel->processGPUPublication();
                EXPECT_LE(publication->counters.size(), 1U);
            }
        });
    gpuSampler.join();
    processSampler.join();
    done.store(true, std::memory_order_release);
    reader.join();

    EXPECT_EQ(gpuModel->processGPUPublication()->version, static_cast<std::uint64_t>(ITERATIONS));
    gpuModel->refresh();
    processModel.refresh();
    EXPECT_EQ(processModel.snapshots().at(0).gpuMemoryBytes, GPU_PROCESS_MEMORY);
}
// Edge case: GPU counters with empty list (no GPUs found)
TEST(ProcessModelTest, MergeGPUDataWithEmptyCounters)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    // Don't add any GPU data - empty counters

    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);

    gpuModel->refresh();
    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // Should have zero GPU data
    EXPECT_EQ(snaps[0].gpuMemoryBytes, 0);
    EXPECT_EQ(snaps[0].gpuUtilPercent, 0.0);
    EXPECT_TRUE(snaps[0].gpuDevices.empty());
}

// Edge case: GPU name lookup fails (ID doesn't match any known GPU)
TEST(ProcessModelTest, MergeGPUDataWithUnknownGPUId)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Known GPU", "TestVendor");
    // Add process GPU data with mismatched GPU ID
    gpuProbe->withProcessGPU(100, "GPU99", 512ULL * 1024 * 1024);

    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);

    processModel.refresh(); // Seed: the processes are listed before the GPU read, as in the app (#1417)
    gpuModel->refresh();
    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // Should have GPU memory, and gpuDevices falls back to GPU ID when name lookup fails
    EXPECT_GT(snaps[0].gpuMemoryBytes, 0);
    EXPECT_EQ(snaps[0].gpuDevices, "GPU99"); // Falls back to GPU ID when name not found
}

// Edge case: Multiple GPU entries with same PID but different GPU IDs (multi-GPU aggregation)
TEST(ProcessModelTest, MergeGPUDataWithLUIDBasedMatching)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "GPU 0", "Vendor").withGPU("GPU1", "GPU 1", "Vendor");
    // Same PID using two different GPUs
    gpuProbe->withProcessGPU(100, "GPU0", 512ULL * 1024 * 1024);
    gpuProbe->withProcessGPU(100, "GPU1", 256ULL * 1024 * 1024);

    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);

    processModel.refresh(); // Seed: the processes are listed before the GPU read, as in the app (#1417)
    gpuModel->refresh();
    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // Should aggregate memory from both GPUs
    EXPECT_EQ(snaps[0].gpuMemoryBytes, 768ULL * 1024 * 1024); // 512 + 256 MB
    // Should include both GPU names
    EXPECT_FALSE(snaps[0].gpuDevices.empty());
}

// #1395: per-process GPU usage reported under an adapter's LUID-based ID (as PDH names adapters on
// Windows) is matched to the adapter all the same: its name and integrated flag.
TEST(ProcessModelTest, MergeGPUDataMatchesAnAdapterByItsLuid)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Integrated GPU", "Vendor", /*isIntegrated=*/true).withLuid("GPU0", "GPU_0x00000000_0x0000F78E");
    gpuProbe->withProcessGPU(100, "GPU_0x00000000_0x0000F78E", 64ULL * 1024 * 1024);

    const auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);
    processModel.refresh();
    gpuModel->refresh();
    processModel.refresh();

    const auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1U);
    ASSERT_EQ(snaps[0].perGpuUsage.size(), 1U);
    EXPECT_EQ(snaps[0].perGpuUsage[0].gpuName, "Integrated GPU");
    EXPECT_TRUE(snaps[0].perGpuUsage[0].isIntegrated);
}

// The per-GPU breakdown carries each adapter's integrated flag, not just its name. It used to be
// left at its default, so the process details pane called every adapter "Discrete" (#963).
TEST(ProcessModelTest, MergeGPUDataCarriesIntegratedFlagPerGpu)
{
    auto processProbe = std::make_unique<MockProcessProbe>();
    processProbe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    processProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Integrated GPU", "Vendor", /*isIntegrated=*/true).withGPU("GPU1", "Discrete GPU", "Vendor", false);
    gpuProbe->withProcessGPU(100, "GPU0", 512ULL * 1024 * 1024);
    gpuProbe->withProcessGPU(100, "GPU1", 256ULL * 1024 * 1024);
    // An adapter the GPU model does not know: there is nothing to look the flag up from, so it
    // must stay at the default rather than inherit a neighbour's.
    gpuProbe->withProcessGPU(100, "GPU99", 128ULL * 1024 * 1024);

    const auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);

    processModel.refresh(); // Seed: the processes are listed before the GPU read, as in the app (#1417)
    gpuModel->refresh();
    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    ASSERT_EQ(snaps[0].perGpuUsage.size(), 3);

    for (const auto& usage : snaps[0].perGpuUsage)
    {
        if (usage.gpuId == "GPU0")
        {
            EXPECT_EQ(usage.gpuName, "Integrated GPU");
            EXPECT_TRUE(usage.isIntegrated);
        }
        else if (usage.gpuId == "GPU1")
        {
            EXPECT_EQ(usage.gpuName, "Discrete GPU");
            EXPECT_FALSE(usage.isIntegrated);
        }
        else
        {
            EXPECT_EQ(usage.gpuId, "GPU99");
            EXPECT_FALSE(usage.isIntegrated);
        }
    }
}

// =============================================================================
// Snapshot Version Tests
// =============================================================================

TEST(ProcessModelTest, WhenConstructed_ThenSnapshotVersionIsZero)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    EXPECT_EQ(model.snapshotVersion(), 0);
}

TEST(ProcessModelTest, WhenRefreshedOnce_ThenSnapshotVersionIsOne)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "proc", 'R', 1000, 500)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.snapshotVersion(), 1);
}

TEST(ProcessModelTest, CopiesSystemHistoriesOnlyForNewerVersion)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "proc", 'R', 1000, 500)});
    probe->setTotalCpuTime(10000);
    Domain::ProcessModel model(std::move(probe));

    Domain::ProcessSystemHistories histories;
    EXPECT_FALSE(model.tryCopySystemHistoriesIfNewer(0, histories));

    model.refresh();

    ASSERT_TRUE(model.tryCopySystemHistoriesIfNewer(0, histories));
    EXPECT_EQ(histories.version, 1);
    EXPECT_EQ(histories.timestamps.size(), histories.power.size());
    EXPECT_EQ(histories.timestamps.size(), histories.pageFaults.size());
    EXPECT_FALSE(model.tryCopySystemHistoriesIfNewer(histories.version, histories));
}

TEST(ProcessModelTest, WhenRefreshedMultipleTimes_ThenSnapshotVersionMonotonicallyIncreases)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "proc", 'R', 1000, 500)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));

    const std::uint64_t versionBefore = model.snapshotVersion();
    model.refresh();
    const std::uint64_t versionAfterFirst = model.snapshotVersion();
    model.refresh();
    const std::uint64_t versionAfterSecond = model.snapshotVersion();

    EXPECT_LT(versionBefore, versionAfterFirst);
    EXPECT_LT(versionAfterFirst, versionAfterSecond);
}
