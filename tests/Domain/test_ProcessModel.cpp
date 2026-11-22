/// @file test_ProcessModel.cpp
/// @brief Domain::ProcessModel: construction, capabilities, CPU percentages, PID reuse and unique keys, state translation and snapshot
/// mapping. Split from test_ProcessModel.cpp by feature (#3850); the shared helpers are in ProcessModelTestSupport.h.

#include "Domain/GPUModel.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/ProcessState.h"
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
// Construction Tests
// =============================================================================

TEST(ProcessModelTest, WhenConstructedWithValidProbe_ThenStartsEmpty)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    EXPECT_EQ(model.processCount(), 0);
    EXPECT_TRUE(model.snapshots().empty());
}

TEST(ProcessModelTest, WhenConstructedWithNullProbe_ThenDoesNotCrash)
{
    Domain::ProcessModel model(nullptr);
    model.refresh(); // Should not crash

    EXPECT_EQ(model.processCount(), 0);
}

TEST(ProcessModelTest, UpdateFromCountersPublishesSnapshotsWithoutProbe)
{
    Domain::ProcessModel model(nullptr);

    model.updateFromCounters({makeCounter(100, "injected", 'R', 1000, 500)}, 100000);

    const auto snapshots = model.snapshots();
    ASSERT_EQ(snapshots.size(), 1);
    EXPECT_EQ(snapshots[0].name, "injected");
    EXPECT_DOUBLE_EQ(snapshots[0].cpuPercent, 0.0);
    EXPECT_EQ(model.snapshotVersion(), 1);
}

TEST(ProcessModelTest, ProbeSuppliedControlCharactersNeverReachASnapshot)
{
    // A process controls its own argv, and a newline inside an argument survives both
    // /proc/[pid]/cmdline (NUL is only the argument separator) and the Windows PEB command line.
    // Rendered verbatim it makes a multi-line table cell, which grows that row taller than the rest
    // and breaks the uniform-row-height assumption ImGuiListClipper relies on (#919). Sanitizing
    // lives here in the Domain layer so both platform probes are covered by one rule.
    Domain::ProcessModel model(nullptr);
    auto counter = makeCounter(100, "na\nme", 'R', 1000, 500);
    counter.command = "bash -c printf \"a\nb\"\tand\rmore";

    model.updateFromCounters({counter}, 100000);

    const auto snapshots = model.snapshots();
    ASSERT_EQ(snapshots.size(), 1);
    EXPECT_EQ(snapshots[0].command, "bash -c printf \"a b\" and more");
    EXPECT_EQ(snapshots[0].name, "na me");
    EXPECT_EQ(snapshots[0].command.find('\n'), std::string::npos);
    EXPECT_EQ(snapshots[0].name.find('\n'), std::string::npos);
}

TEST(ProcessModelTest, AnIllFormedUtf8NameOrCommandIsShownWithReplacementCharacters)
{
    // #4301: Linux keeps 15 bytes of a comm, which can end in the middle of a character, and a command line can
    // hold a Latin-1 file name. The snapshot (what the UI shows) gets U+FFFD per maximal ill-formed subpart;
    // the raw counters, which actions and the crash-history comm match use, are not the snapshot's concern.
    Domain::ProcessModel model(nullptr);
    const std::string cutComm = std::string("QQ\xE9\x9F\xB3\xE4\xB9\x90\xE6\x92\xAD\xE6\x94\xBE\xE5");
    ASSERT_EQ(cutComm.size(), 15U);
    auto counter = makeCounter(100, cutComm, 'R', 1000, 500);
    counter.command = "vlc Gr\xFC\xDF"
                      "e.mp3";

    model.updateFromCounters({counter}, 100000);

    const auto snapshots = model.snapshots();
    ASSERT_EQ(snapshots.size(), 1);
    EXPECT_EQ(snapshots[0].name, "QQ\xE9\x9F\xB3\xE4\xB9\x90\xE6\x92\xAD\xE6\x94\xBE\xEF\xBF\xBD");
    EXPECT_EQ(snapshots[0].command,
              "vlc Gr\xEF\xBF\xBD\xEF\xBF\xBD"
              "e.mp3");
    EXPECT_TRUE(Domain::isValidUtf8(snapshots[0].name));
    EXPECT_TRUE(Domain::isValidUtf8(snapshots[0].command));
}

TEST(ProcessModelTest, ASingleLineFormFollowsItsProcessAcrossRefreshes)
{
    // The single-line form is kept per process and re-derived only when the raw string changes
    // (#1624): a process that renames itself, or rewrites its argv, must still show the new text,
    // sanitized, and an unchanged one its old sanitized form.
    Domain::ProcessModel model(nullptr);
    auto counter = makeCounter(100, "na\nme", 'R', 1000, 500);
    counter.command = "plain command";
    auto other = makeCounter(200, "steady", 'R', 1000, 500);
    other.command = "steady\tcommand";

    std::uint64_t totalCpuTime = 100000;
    const auto refresh = [&]
    {
        model.updateFromCounters({counter, other}, totalCpuTime);
        totalCpuTime += 1000;
        return model.snapshots();
    };
    const auto find = [](const std::vector<Domain::ProcessSnapshot>& snapshots, std::int32_t pid)
    {
        return *std::ranges::find_if(snapshots, [pid](const auto& snapshot) { return snapshot.pid == pid; });
    };

    auto snapshots = refresh();
    EXPECT_EQ(find(snapshots, 100).name, "na me");
    EXPECT_EQ(find(snapshots, 100).command, "plain command");

    snapshots = refresh(); // Unchanged: the kept forms.
    EXPECT_EQ(find(snapshots, 100).name, "na me");
    EXPECT_EQ(find(snapshots, 100).command, "plain command");
    EXPECT_EQ(find(snapshots, 200).command, "steady command");

    counter.name = "renamed";
    counter.command = "now\r\nmulti-line";
    snapshots = refresh(); // Both changed: derived again, the dirty one sanitized and the clean one as is.
    EXPECT_EQ(find(snapshots, 100).name, "renamed");
    EXPECT_EQ(find(snapshots, 100).command, "now  multi-line");
    EXPECT_EQ(find(snapshots, 200).name, "steady");
    EXPECT_EQ(find(snapshots, 200).command, "steady command");

    counter.name = "na\nme"; // Back to the first name, which the memo no longer holds.
    snapshots = refresh();
    EXPECT_EQ(find(snapshots, 100).name, "na me");
    EXPECT_EQ(find(snapshots, 100).command, "now  multi-line");

    // The same PID with another start time is another process: nothing carries over.
    counter = makeCounter(100, "fresh\x7F", 'R', 1000, 500, 2000);
    snapshots = refresh();
    EXPECT_EQ(find(snapshots, 100).name, "fresh ");
    EXPECT_EQ(find(snapshots, 100).command, "");
}

TEST(ProcessModelTest, WhenProbeReportsCapabilities_ThenCapabilitiesAreExposed)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCapabilities caps;
    caps.hasIoCounters = true;
    caps.hasThreadCount = true;
    caps.hasUserSystemTime = true;
    caps.hasStartTime = true;
    probe->setCapabilities(caps);

    Domain::ProcessModel model(std::move(probe));

    const auto& modelCaps = model.capabilities();
    EXPECT_TRUE(modelCaps.hasIoCounters);
    EXPECT_TRUE(modelCaps.hasThreadCount);
}

TEST(ProcessModelTest, WhenProbeReportsReducedPrivileges_ThenCapabilitiesReflectThat)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCapabilities caps;
    caps.hasReducedPrivileges = true;
    probe->setCapabilities(caps);

    Domain::ProcessModel model(std::move(probe));

    EXPECT_TRUE(model.capabilities().hasReducedPrivileges);
}

TEST(ProcessModelTest, WhenProbeReportsFullPrivileges_ThenReducedPrivilegesIsFalse)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCapabilities caps;
    caps.hasReducedPrivileges = false;
    probe->setCapabilities(caps);

    Domain::ProcessModel model(std::move(probe));

    EXPECT_FALSE(model.capabilities().hasReducedPrivileges);
}

// #1254: capabilities were copied once at construction, so one the probe withdrew later (Windows'
// EStats check after the first real sample, #1161) never reached the UI. They are re-read every
// sample and published with that generation, through every reader path.
TEST(ProcessModelTest, ACapabilityWithdrawnAfterTheFirstSampleIsPublished)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    Platform::ProcessCapabilities caps;
    caps.hasNetworkCounters = true;
    caps.hasReducedPrivileges = false;
    rawProbe->setCapabilities(caps);
    probe->setCounters({makeCounter(1, "test", 'R', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();
    EXPECT_TRUE(model.capabilities().hasNetworkCounters);
    EXPECT_FALSE(model.capabilities().hasReducedPrivileges);

    Platform::ProcessCapabilities withdrawn = caps;
    withdrawn.hasNetworkCounters = false;
    withdrawn.hasReducedPrivileges = true;
    rawProbe->setCapabilities(withdrawn);
    EXPECT_TRUE(model.capabilities().hasNetworkCounters); // published with a sample, not before it

    model.refresh();
    EXPECT_EQ(model.capabilities(), withdrawn);

    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> snapshots;
    std::uint64_t version = 0;
    Platform::ProcessCapabilities published = caps;
    ASSERT_TRUE(model.tryCopySnapshotsIfNewer(0, snapshots, version, &published));
    EXPECT_EQ(published, withdrawn);

    Domain::ProcessSystemHistories histories;
    ASSERT_TRUE(model.tryCopySystemHistoriesIfNewer(0, histories));
    EXPECT_EQ(histories.capabilities, withdrawn);
}

// #1254: a capability the probe withdraws during the sample itself (the Windows probe does so in
// readSocketTraffic(), #1161) is published with that same generation.
TEST(ProcessModelTest, ACapabilityWithdrawnDuringASampleIsPublishedWithThatSample)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    Platform::ProcessCapabilities caps;
    caps.hasNetworkCounters = true;
    rawProbe->setCapabilities(caps);
    probe->setCounters({makeCounter(1, "test", 'R', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    Platform::ProcessCapabilities withdrawn = caps;
    withdrawn.hasNetworkCounters = false;
    rawProbe->switchCapabilitiesOnNextSocketRead(withdrawn);
    model.refresh();

    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> snapshots;
    std::uint64_t version = 0;
    Platform::ProcessCapabilities published = caps;
    ASSERT_TRUE(model.tryCopySnapshotsIfNewer(0, snapshots, version, &published));
    EXPECT_FALSE(published.hasNetworkCounters);
    ASSERT_EQ(snapshots->size(), 1U);
    EXPECT_FALSE((*snapshots)[0].networkAvailable);
}

// =============================================================================
// ISamplable Tests
// =============================================================================

TEST(ProcessModelTest, SampleDelegatesToRefresh)
{
    // sample() is the ISamplable override BackgroundSampler calls in production;
    // every other test in this file drives refresh() directly, so this is the
    // only coverage of sample()'s own body.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    static_cast<Domain::ISamplable&>(model).sample();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].pid, 100);
}

// =============================================================================
// CPU Percentage Calculation Tests
// =============================================================================

TEST(ProcessModelTest, FirstRefreshShowsZeroCpuPercent)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuPercent, 0.0); // No previous data to compare
}

TEST(ProcessModelTest, CpuPercentCalculatedFromDeltas)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First sample: process has used 1000 user + 500 system = 1500 total
    rawProbe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Second sample: process has used 2000 user + 1000 system = 3000 total
    // Delta = 3000 - 1500 = 1500
    // Total CPU delta = 200000 - 100000 = 100000
    // CPU% = (1500 / 100000) * 100 = 1.5%
    rawProbe->setCounters({makeCounter(100, "test_proc", 'R', 2000, 1000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].cpuPercent, 1.5);
}

TEST(ProcessModelTest, CpuPercentForMultipleProcesses)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First sample: two processes
    rawProbe->setCounters({
        makeCounter(100, "proc_a", 'R', 1000, 0),
        makeCounter(200, "proc_b", 'R', 2000, 0),
    });
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Second sample: proc_a gained 500, proc_b gained 1000
    // Total CPU delta = 100000
    // proc_a: (500 / 100000) * 100 = 0.5%
    // proc_b: (1000 / 100000) * 100 = 1.0%
    rawProbe->setCounters({
        makeCounter(100, "proc_a", 'R', 1500, 0),
        makeCounter(200, "proc_b", 'R', 3000, 0),
    });
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);

    // Find each process
    const Domain::ProcessSnapshot* snapA = nullptr;
    const Domain::ProcessSnapshot* snapB = nullptr;
    for (const auto& s : snaps)
    {
        if (s.pid == 100)
            snapA = &s;
        if (s.pid == 200)
            snapB = &s;
    }

    ASSERT_NE(snapA, nullptr);
    ASSERT_NE(snapB, nullptr);
    EXPECT_DOUBLE_EQ(snapA->cpuPercent, 0.5);
    EXPECT_DOUBLE_EQ(snapB->cpuPercent, 1.0);
}

TEST(ProcessModelTest, CpuPercentZeroWhenNoDelta)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "idle_proc", 'S', 1000, 500)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Second sample: process hasn't used any more CPU
    rawProbe->setCounters({makeCounter(100, "idle_proc", 'S', 1000, 500)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].cpuPercent, 0.0);
}

TEST(ProcessModelTest, CpuPercentZeroWhenTotalCpuDeltaIsZero)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "test_proc", 'R', 1000, 500)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Second sample with same total CPU time (shouldn't happen in practice)
    rawProbe->setCounters({makeCounter(100, "test_proc", 'R', 2000, 1000)});
    rawProbe->setTotalCpuTime(100000); // Same as before
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].cpuPercent, 0.0); // Division by zero avoided
}

TEST(ProcessModelTest, HighCpuPercentageCalculation)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "busy_proc", 'R', 0, 0)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Process uses 50% of total CPU delta
    // Delta = 50000, Total = 100000
    // CPU% = 50%
    rawProbe->setCounters({makeCounter(100, "busy_proc", 'R', 50000, 0)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].cpuPercent, 50.0);
}

// =============================================================================
// PID Reuse / Unique Key Tests
// =============================================================================

TEST(ProcessModelTest, NewProcessWithSamePidGetsZeroCpu)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // Original process PID 100, startTime 1000
    rawProbe->setCounters({makeCounter(100, "original", 'R', 10000, 5000, /*startTime*/ 1000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // New process reuses PID 100 but has different startTime
    rawProbe->setCounters({makeCounter(100, "new_proc", 'R', 100, 50, /*startTime*/ 2000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].name, "new_proc");
    EXPECT_DOUBLE_EQ(snaps[0].cpuPercent, 0.0); // No valid previous data
}

TEST(ProcessModelTest, SameProcessRetainsCpuHistory)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // Process with consistent startTime
    rawProbe->setCounters({makeCounter(100, "persistent", 'R', 1000, 500, /*startTime*/ 1000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Same process (same startTime) with more CPU usage
    rawProbe->setCounters({makeCounter(100, "persistent", 'R', 2000, 1000, /*startTime*/ 1000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuPercent, 1.5); // History preserved
}

TEST(ProcessModelTest, UniqueKeyIsConsistentForSameProcess)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "test", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps1 = model.snapshots();

    // Refresh with same process
    rawProbe->setCounters({makeCounter(100, "test", 'R', 2000, 0, 5000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps2 = model.snapshots();

    EXPECT_EQ(snaps1[0].uniqueKey, snaps2[0].uniqueKey);
}

TEST(ProcessModelTest, UniqueKeyDiffersForPidReuse)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "proc_v1", 'R', 1000, 0, /*startTime*/ 1000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps1 = model.snapshots();

    // New process with same PID but different start time
    rawProbe->setCounters({makeCounter(100, "proc_v2", 'R', 100, 0, /*startTime*/ 2000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps2 = model.snapshots();

    EXPECT_NE(snaps1[0].uniqueKey, snaps2[0].uniqueKey);
}

TEST(ProcessModelTest, UniqueKeyDiffersForPairsTheOldHashCollidedOn)
{
    // Pairs the boost-style combine (with the identity std::hash of libstdc++ and libc++) mapped to one
    // key (#2115, #2158): adjacent PIDs forked in the same tick, and two found by search.
    struct Identity
    {
        std::int32_t pid;
        std::uint64_t startTime;
    };
    const std::vector<std::pair<Identity, Identity>> pairs = {
        {{.pid = 895, .startTime = 7}, {.pid = 896, .startTime = 7}},
        {{.pid = 16095, .startTime = 2152828}, {.pid = 16389, .startTime = 2152828}},
        {{.pid = 1000, .startTime = 50000}, {.pid = 1001, .startTime = 49935}},
    };
    for (const auto& [a, b] : pairs)
    {
        auto probe = std::make_unique<MockProcessProbe>();
        probe->setCounters({makeCounter(a.pid, "a", 'R', 0, 0, a.startTime), makeCounter(b.pid, "b", 'R', 0, 0, b.startTime)});
        probe->setTotalCpuTime(100000);
        Domain::ProcessModel model(std::move(probe));
        model.refresh();

        const auto snaps = model.snapshots();
        ASSERT_EQ(snaps.size(), 2U);
        EXPECT_NE(snaps[0].uniqueKey, snaps[1].uniqueKey) << a.pid << "@" << a.startTime << " vs " << b.pid << "@" << b.startTime;
    }
}

TEST(ProcessModelTest, UniqueKeysOfAForkBurstAreDistinct)
{
    // A fork burst: 2000 consecutive PIDs, a handful of processes started per clock tick (#2115).
    auto probe = std::make_unique<MockProcessProbe>();
    constexpr std::int32_t COUNT = 2000;
    std::vector<Platform::ProcessCounters> counters;
    counters.reserve(static_cast<std::size_t>(COUNT));
    for (std::int32_t i = 0; i < COUNT; ++i)
    {
        counters.push_back(makeCounter(1000 + i, "burst", 'R', 0, 0, 5000 + static_cast<std::uint64_t>(i / 4)));
    }
    probe->setCounters(counters);
    probe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    std::unordered_set<std::uint64_t> keys;
    for (const auto& snap : model.snapshots())
    {
        keys.insert(snap.uniqueKey);
    }
    EXPECT_EQ(keys.size(), static_cast<std::size_t>(COUNT));
}

TEST(ProcessModelTest, TryCopySnapshotsIfNewerOnlyCopiesWhenVersionAdvances)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "test", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto initialVersion = model.snapshotVersion();
    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> copiedSnapshots;
    std::uint64_t copiedVersion = 0;

    EXPECT_FALSE(model.tryCopySnapshotsIfNewer(initialVersion, copiedSnapshots, copiedVersion));

    rawProbe->setCounters({makeCounter(100, "test", 'R', 2000, 0, 5000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    EXPECT_TRUE(model.tryCopySnapshotsIfNewer(initialVersion, copiedSnapshots, copiedVersion));
    EXPECT_EQ(copiedVersion, model.snapshotVersion());
    ASSERT_TRUE(copiedSnapshots != nullptr);
    ASSERT_EQ(copiedSnapshots->size(), 1);
}

TEST(ProcessModelTest, TryCopySnapshotsIfNewerHandsOutSharedVectorNotADeepCopy)
{
    // Regression test for #843 Phase 3b: tryCopySnapshotsIfNewer() used to deep-copy every
    // process (including strings/nested data) into outSnapshots under a shared_lock,
    // contending with the writer's next unique_lock for however long that copy took. It now
    // hands out a shared_ptr to ProcessModel's own immutable published vector instead, so two
    // callers that both fetch the same generation must observe the identical vector object
    // (same address), not two independently-copied ones -- proving no deep copy happened.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setCounters({makeCounter(100, "test", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> first;
    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> second;
    std::uint64_t firstVersion = 0;
    std::uint64_t secondVersion = 0;

    ASSERT_TRUE(model.tryCopySnapshotsIfNewer(0, first, firstVersion));
    ASSERT_TRUE(model.tryCopySnapshotsIfNewer(0, second, secondVersion));

    EXPECT_EQ(first.get(), second.get()) << "two readers of the same generation should share the identical vector object";
    EXPECT_GT(first.use_count(), 1) << "the model itself should still hold a reference alongside the readers'";
}

TEST(ProcessModelTest, FindSnapshotReturnsMatchingProcessAmongMultiple)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({
        makeCounter(100, "alpha", 'R', 1000, 0, 5000),
        makeCounter(200, "beta", 'S', 2000, 0, 6000),
        makeCounter(300, "gamma", 'R', 3000, 0, 7000),
    });
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto found = model.findSnapshot(200);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->pid, 200);
    EXPECT_EQ(found->name, "beta");
}

TEST(ProcessModelTest, FindSnapshotReturnsNullForUnknownPid)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "test", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.findSnapshot(999), nullptr);
}

TEST(ProcessModelTest, FindSnapshotReflectsLatestRefreshEvenWithoutCopyingFullVector)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "test", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto before = model.findSnapshot(100);
    ASSERT_NE(before, nullptr);
    EXPECT_DOUBLE_EQ(before->cpuPercent, 0.0);

    rawProbe->setCounters({makeCounter(100, "test", 'R', 2000, 0, 5000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    const auto after = model.findSnapshot(100);
    ASSERT_NE(after, nullptr);
    EXPECT_GT(after->cpuPercent, 0.0);
    EXPECT_DOUBLE_EQ(before->cpuPercent, 0.0) << "a held result keeps its own generation's row";
}

// #3601: findSnapshot() shares the published row instead of copying it.
TEST(ProcessModelTest, FindSnapshotSharesThePublishedRowInsteadOfCopyingIt)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "alpha", 'R', 1000, 0, 5000), makeCounter(200, "beta", 'S', 2000, 0, 6000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto found = model.findSnapshot(200);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found.get(), model.findSnapshot(200).get()) << "two lookups share one row";

    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> rows;
    std::uint64_t version = 0;
    ASSERT_TRUE(model.tryCopySnapshotsIfNewer(0, rows, version));
    ASSERT_NE(rows, nullptr);
    const auto published = std::ranges::find(*rows, 200, &Domain::ProcessSnapshot::pid);
    ASSERT_NE(published, rows->end());
    EXPECT_EQ(found.get(), &*published) << "the row is the published generation's own";
}

// =============================================================================
// State Translation Tests
// =============================================================================

TEST(ProcessModelTest, GivenRunningState_WhenRefreshed_ThenSnapshotStateIsRunning)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", 'R', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Running");
}

TEST(ProcessModelTest, GivenSleepingState_WhenRefreshed_ThenSnapshotStateIsSleeping)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", 'S', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Sleeping");
}

TEST(ProcessModelTest, GivenDiskSleepState_WhenRefreshed_ThenSnapshotStateIsDiskSleep)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", 'D', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Disk Sleep");
}

TEST(ProcessModelTest, GivenZombieState_WhenRefreshed_ThenSnapshotStateIsZombie)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", 'Z', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Zombie");
}

TEST(ProcessModelTest, GivenStoppedState_WhenRefreshed_ThenSnapshotStateIsStopped)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", 'T', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, Domain::processStateName('T')); // Suspended on Windows (#4755)
}

TEST(ProcessModelTest, GivenUnknownState_WhenRefreshed_ThenSnapshotStateIsUnknown)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "test", '?', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Unknown");
}

TEST(ProcessModelTest, GivenTracingState_WhenRefreshed_ThenSnapshotStateIsTracing)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "debugged_proc", 't', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Tracing");
}

TEST(ProcessModelTest, GivenDeadState_WhenRefreshed_ThenSnapshotStateIsDead)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "dead_proc", 'X', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Dead");
}

TEST(ProcessModelTest, GivenIdleState_WhenRefreshed_ThenSnapshotStateIsIdle)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "idle_kernel_thread", 'I', 0, 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    EXPECT_EQ(snaps[0].displayState, "Idle");
}

// =============================================================================
// Snapshot Data Mapping Tests
// =============================================================================

TEST(ProcessModelTest, SnapshotContainsAllFields)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters c;
    c.pid = 12345;
    c.parentPid = 100;
    c.name = "my_process";
    c.state = 'S';
    c.userTime = 1000;
    c.systemTime = 500;
    c.startTimeTicks = 9999;
    c.rssBytes = 1024 * 1024 * 50; // 50 MB
    c.virtualBytes = 1024 * 1024 * 200;
    c.threadCount = 4;

    probe->setCounters({c});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    const auto& snap = snaps[0];
    EXPECT_EQ(snap.pid, 12345);
    EXPECT_EQ(snap.parentPid, 100);
    EXPECT_EQ(snap.name, "my_process");
    EXPECT_EQ(snap.displayState, "Sleeping");
}

// CPU Affinity Tests
// =============================================================================

TEST(ProcessModelTest, CpuAffinityIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "affinity_test", 'R', 1000, 500);
    counter.cpuAffinity = Platform::CpuAffinity::fromMask(0x0F); // Cores 0-3
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuAffinity, Platform::CpuAffinity::fromMask(0x0F));
}

TEST(ProcessModelTest, CpuAffinityEmptyWhenNotAvailable)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "no_affinity", 'R', 1000, 500);
    // cpuAffinity left empty: not available
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_TRUE(snaps[0].cpuAffinity.empty());
}

TEST(ProcessModelTest, CpuAffinityAllCores)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "all_cores", 'R', 1000, 500);
    counter.cpuAffinity = Platform::CpuAffinity::fromMask(0xFFFFFFFFFFFFFFFF); // All 64 cores
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuAffinity, Platform::CpuAffinity::fromMask(0xFFFFFFFFFFFFFFFF));
}

// #1247: processors at 64 and above reach the snapshot instead of being cut off at 64 bits.
TEST(ProcessModelTest, CpuAffinityBeyond64CoresIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "pinned_high", 'R', 1000, 500);
    counter.cpuAffinity = *Platform::CpuAffinity::fromCpuList("0-3,70,128-255");
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuAffinity, *Platform::CpuAffinity::fromCpuList("0-3,70,128-255"));
    EXPECT_TRUE(snaps[0].cpuAffinity.test(70));
    EXPECT_EQ(snaps[0].cpuAffinity.count(), 4U + 1U + 128U);
}
