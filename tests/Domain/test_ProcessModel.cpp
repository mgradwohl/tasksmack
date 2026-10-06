/// @file test_ProcessModel.cpp
/// @brief Comprehensive tests for Domain::ProcessModel
///
/// Tests cover:
/// - CPU percentage calculations from counter deltas
/// - Snapshot data transformation
/// - State character translation
/// - Unique key generation for PID reuse handling
/// - Thread-safe operations

#include "Domain/GPUModel.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/GPUTypes.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Use shared mock from TestMocks namespace
using TestMocks::makeProcessCounters;
using TestMocks::MockProcessProbe;

namespace
{

// Deterministic time source for tests that need a known interval between refreshes,
// so rates and power are exact instead of depending on how long a sleep took (#1136).
class ManualClock
{
  public:
    [[nodiscard]] Domain::ProcessModel::NowFunction now()
    {
        return [this]
        {
            return m_Time;
        };
    }

    void advance(Domain::ProcessModel::Clock::duration duration)
    {
        m_Time += duration;
    }

  private:
    Domain::ProcessModel::Clock::time_point m_Time;
};

// Test constants for overflow scenarios
constexpr uint64_t OVERFLOW_TEST_MARGIN = 10000; // Distance from max value for overflow tests

/// Helper to create a process counter (legacy compatibility wrapper).
Platform::ProcessCounters makeCounter(int32_t pid,
                                      const std::string& name,
                                      char state,
                                      uint64_t userTime,
                                      uint64_t systemTime,
                                      uint64_t startTime = 1000,
                                      uint64_t rssBytes = 1024 * 1024,
                                      int32_t parentPid = 1)
{
    return makeProcessCounters(pid, name, state, userTime, systemTime, startTime, rssBytes, parentPid);
}

} // namespace

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
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->pid, 200);
    EXPECT_EQ(found->name, "beta");
}

TEST(ProcessModelTest, FindSnapshotReturnsNulloptForUnknownPid)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "test", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_FALSE(model.findSnapshot(999).has_value());
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
    ASSERT_TRUE(before.has_value());
    EXPECT_DOUBLE_EQ(before->cpuPercent, 0.0);

    rawProbe->setCounters({makeCounter(100, "test", 'R', 2000, 0, 5000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    const auto after = model.findSnapshot(100);
    ASSERT_TRUE(after.has_value());
    EXPECT_GT(after->cpuPercent, 0.0);
}

TEST(ProcessModelTest, FindSnapshotWithVersionReturnsSnapshotAndCurrentVersionTogether)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "test", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto found = model.findSnapshotWithVersion(100);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->snapshot.pid, 100);
    EXPECT_EQ(found->version, model.snapshotVersion());
}

TEST(ProcessModelTest, FindSnapshotWithVersionReturnsNulloptForUnknownPid)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "test", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_FALSE(model.findSnapshotWithVersion(999).has_value());
}

TEST(ProcessModelTest, DemonstratesSeparateFindSnapshotAndSnapshotVersionCallsAreUnsafe)
{
    // Illustrative only -- deliberately NOT a regression test for findSnapshotWithVersion()
    // itself (an earlier version of this test incorrectly claimed to be one; a review
    // caught the error). A single-threaded test can never make a refresh() land "during" a
    // call, atomic or not: calls in one thread execute strictly in sequence, so there is no
    // sequence of calls here that would look any different if findSnapshotWithVersion() were
    // reverted to two separate calls -- that regression can only be caught by genuine
    // concurrency. FindSnapshotWithVersionNeverPairsSnapshotFromOneGenerationWithVersionFromAnother
    // below, which races two real threads, is the only test that actually guards it.
    //
    // What this test shows instead, deterministically and without any race: calling
    // findSnapshot() and a separate, later snapshotVersion() -- generically, the same
    // "split read" shape as ShellLayer's old bug -- really can pair a snapshot from one
    // generation with the version of a different one, just by having an ordinary refresh()
    // happen between the two calls. This is a simplified illustration of the hazard class,
    // not a literal replay of ShellLayer's exact former call sequence: ShellLayer actually
    // paired ProcessModel::findSnapshot()'s always-fresh result with
    // ProcessesPanel::cachedSnapshotVersion() (a separate render-cache value that could lag
    // behind, only refreshed while the Processes tab was active) -- see
    // src/App/ShellLayer.cpp's history around the #855 fix -- so the real bug's typical
    // failure direction was a FRESH snapshot paired with a STALE version, the opposite of
    // the OLD-snapshot/NEW-version pairing demonstrated below. Both directions are instances
    // of the same underlying "two separate non-atomic reads" hazard that
    // findSnapshotWithVersion() closes; this test just uses ProcessModel's own two accessors
    // for a self-contained, dependency-free demonstration.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setCounters({makeCounter(100, "gen0", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh(); // publishes generation 0
    const auto versionAfterGen0 = model.snapshotVersion();

    // Two separate calls -- findSnapshot() then, later, an independent snapshotVersion() --
    // illustrating the split-read shape generically (see the class-level comment above for
    // how this differs from ShellLayer's exact former call sequence).
    const auto separateSnapshot = model.findSnapshot(100);
    ASSERT_TRUE(separateSnapshot.has_value());
    EXPECT_EQ(separateSnapshot->name, "gen0");

    // The gap: an ordinary refresh() lands between the two separate calls.
    rawProbe->setCounters({makeCounter(100, "gen1", 'R', 2000, 0, 5000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh(); // publishes generation 1

    const auto separateVersion = model.snapshotVersion();

    // The mismatch, made concrete: separateSnapshot still holds generation 0's content (read
    // before the refresh), but separateVersion has advanced past versionAfterGen0 -- it now
    // reflects generation 1, published after separateSnapshot was captured. A caller pairing
    // (separateSnapshot, separateVersion) this way would see generation 1's version attached
    // to generation 0's content -- the same category of mismatch findSnapshotWithVersion()
    // exists to make impossible, even though it doesn't literally replay ShellLayer's exact
    // old values.
    EXPECT_EQ(separateSnapshot->name, "gen0");
    EXPECT_GT(separateVersion, versionAfterGen0) << "expected the version to have advanced to reflect generation 1, "
                                                    "while separateSnapshot still holds generation 0's content";
}

TEST(ProcessModelTest, FindSnapshotWithVersionAdvancesVersionAfterEachRefresh)
{
    // Sanity check only: confirms the version advances and the snapshot content moves
    // together across two sequential refresh() calls. This does NOT exercise the race
    // findSnapshotWithVersion() exists to close -- see
    // FindSnapshotWithVersionNeverPairsSnapshotFromOneGenerationWithVersionFromAnother below
    // for the test that actually reproduces the old split-read bug (via genuine
    // concurrency; a single-threaded test cannot force a refresh() to land "during" any
    // call, so no sequential test can regression-test this method's atomicity), or
    // DemonstratesSeparateFindSnapshotAndSnapshotVersionCallsAreUnsafe above for a
    // deterministic (but illustrative-only) look at the failure mode being closed.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({makeCounter(100, "test", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto before = model.findSnapshotWithVersion(100);
    ASSERT_TRUE(before.has_value());
    EXPECT_DOUBLE_EQ(before->snapshot.cpuPercent, 0.0);

    rawProbe->setCounters({makeCounter(100, "test", 'R', 2000, 0, 5000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    const auto after = model.findSnapshotWithVersion(100);
    ASSERT_TRUE(after.has_value());
    EXPECT_GT(after->snapshot.cpuPercent, 0.0);
    EXPECT_GT(after->version, before->version);
}

TEST(ProcessModelTest, FindSnapshotWithVersionNeverPairsSnapshotFromOneGenerationWithVersionFromAnother)
{
    // Reproduces the ShellLayer race under concurrency: a background writer thread
    // continuously republishes new generations while a reader thread concurrently calls
    // findSnapshotWithVersion(). Each generation's process name encodes which generation
    // produced it (name is copied verbatim into the snapshot -- see
    // ProcessModel.cpp:computeSnapshot() `snapshot.name = current.name;`), and the writer
    // records which published version corresponds to which generation. If snapshot and
    // version were ever read as two separate, non-atomic steps (the actual bug: ShellLayer
    // called findSnapshot() then a separate cachedSnapshotVersion()), a reader could observe
    // a name from one generation paired with the version published for a different
    // generation. This test would fail if findSnapshotWithVersion() were reimplemented as
    // two separate locked calls instead of one, unlike the sequential test above.
    //
    // Note on rigor: the checkpoint handshake below guarantees a minimum number of reader
    // observations, but each guaranteed observation happens once the writer is already
    // paused waiting for it -- so, in principle, an adversarial scheduler could satisfy the
    // checkpoints without ever running the reader concurrently with an in-flight write.
    // DemonstratesSeparateFindSnapshotAndSnapshotVersionCallsAreUnsafe above does NOT close
    // that gap (an earlier version of this comment incorrectly claimed it did; a review
    // caught the error): it's a single-threaded test, and a single thread can never make a
    // refresh() land "during" any call, atomic or not, so it cannot regression-test
    // findSnapshotWithVersion()'s atomicity -- it only illustrates why the old two-call
    // pattern is unsafe. This test, exercising two real racing threads, remains the sole
    // regression detector for findSnapshotWithVersion() itself; the adversarial-scheduler
    // caveat above is a real, currently-unclosed limitation of that detector, not one this
    // suite has a production-code-test-seam-free way to eliminate (see the PR discussion on
    // #864 for why one wasn't added).
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));

    constexpr std::int32_t kPid = 100;
    constexpr int kIterations = 2000;
    // The writer pauses for the reader every kCheckpointInterval generations (see below):
    // deterministic pacing, not a probability tuned against past CI flakiness.
    constexpr int kCheckpointInterval = 10;
    constexpr auto kCheckpointTimeout = std::chrono::seconds(5);

    std::mutex registryMutex;
    std::unordered_map<std::uint64_t, std::string> versionToName;

    std::mutex checkpointMutex;
    std::condition_variable checkpointCv;
    int readerObservationCount = 0; // guarded by checkpointMutex
    std::atomic<bool> checkpointTimedOut{false};

    std::atomic<bool> writerDone{false};

    std::thread writer(
        [&]
        {
            for (int i = 0; i < kIterations; ++i)
            {
                std::string name = "gen_" + std::to_string(i);
                rawProbe->setCounters({makeCounter(kPid, name, 'R', static_cast<uint64_t>(i) * 1000, 0, 5000)});
                model.refresh();
                const auto version = model.snapshotVersion();
                {
                    const std::scoped_lock lock(registryMutex);
                    versionToName.emplace(version, std::move(name));
                }

                // Deterministic pacing, not incidental OS scheduling luck: block until the
                // reader records at least one more successful observation than it had at the
                // start of this batch. C++ gives no fairness guarantee between these two
                // threads, so without an explicit handshake like this, the writer could in
                // principle finish all kIterations generations before the reader ever gets a
                // time slice -- making any fixed-ratio liveness threshold still flaky in
                // principle, however unlikely in practice (as raised in review on #864).
                if ((i + 1) % kCheckpointInterval == 0)
                {
                    std::unique_lock checkpointLock(checkpointMutex);
                    const int target = readerObservationCount + 1;
                    const bool reachedTarget =
                        checkpointCv.wait_for(checkpointLock, kCheckpointTimeout, [&] { return readerObservationCount >= target; });
                    if (!reachedTarget)
                    {
                        // Don't hang the test process indefinitely if the reader is stuck
                        // (e.g. findSnapshotWithVersion() never finds the pid due to some
                        // unrelated regression) -- stop publishing and let the assertion below
                        // fail with a clear message instead.
                        checkpointTimedOut.store(true);
                        break;
                    }
                }
            }
            writerDone.store(true);
        });

    std::vector<std::pair<std::uint64_t, std::string>> observed;
    while (!writerDone.load())
    {
        if (const auto found = model.findSnapshotWithVersion(kPid); found.has_value())
        {
            observed.emplace_back(found->version, found->snapshot.name);
            const std::scoped_lock checkpointLock(checkpointMutex);
            ++readerObservationCount;
            checkpointCv.notify_all();
        }
    }
    // One final read after the writer stops, to also cover the last published generation.
    if (const auto found = model.findSnapshotWithVersion(kPid); found.has_value())
    {
        observed.emplace_back(found->version, found->snapshot.name);
    }

    writer.join();

    ASSERT_FALSE(checkpointTimedOut.load()) << "the reader never caught up to a writer checkpoint within the deadline -- "
                                            << "findSnapshotWithVersion() may be broken (e.g. never finding the pid)";

    // Deterministic liveness guarantee (not a probabilistic threshold): the checkpoint
    // handshake above guarantees the reader at least one successful observation every
    // kCheckpointInterval generations, so it must have recorded at least kIterations /
    // kCheckpointInterval observations by the time the writer finishes -- regardless of how
    // the OS happens to schedule these two threads.
    EXPECT_GE(observed.size(), static_cast<std::size_t>(kIterations / kCheckpointInterval))
        << "reader only recorded " << observed.size() << " observations; the writer's checkpoint pacing should have "
        << "guaranteed at least " << (kIterations / kCheckpointInterval);

    ASSERT_FALSE(observed.empty());
    const std::scoped_lock lock(registryMutex);
    for (const auto& [version, name] : observed)
    {
        const auto entry = versionToName.find(version);
        ASSERT_TRUE(entry != versionToName.end()) << "version " << version << " was never recorded by the writer";
        EXPECT_EQ(entry->second, name) << "version " << version << " observed with a mismatched process name";
    }
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
    EXPECT_EQ(snaps[0].displayState, "Stopped");
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
    counter.cpuAffinityMask = 0x0F; // Cores 0-3
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuAffinityMask, 0x0F);
}

TEST(ProcessModelTest, CpuAffinityZeroWhenNotAvailable)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "no_affinity", 'R', 1000, 500);
    counter.cpuAffinityMask = 0; // Not available
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuAffinityMask, 0);
}

TEST(ProcessModelTest, CpuAffinityAllCores)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "all_cores", 'R', 1000, 500);
    counter.cpuAffinityMask = 0xFFFFFFFFFFFFFFFF; // All 64 cores
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].cpuAffinityMask, 0xFFFFFFFFFFFFFFFF);
}

// =============================================================================
// Network Rate Calculation Tests
// =============================================================================

TEST(ProcessModelTest, NetworkRatesZeroOnFirstRefresh)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(100, "network_proc").withNetworkCounters(100, 1000, 2000);
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].netSentBytesPerSec, 0.0);     // No previous data
    EXPECT_DOUBLE_EQ(snaps[0].netReceivedBytesPerSec, 0.0); // No previous data
}

namespace
{

// Drives a ProcessModel's sample times through the injectable clock, so the network rate tests
// get exact intervals instead of depending on a real sleep.
struct NetworkRateFixture
{
    Domain::ProcessModel::Clock::time_point currentTime;
    MockProcessProbe* probe = nullptr;
    std::unique_ptr<Domain::ProcessModel> model;
    std::uint64_t totalCpuTime = 100000;

    NetworkRateFixture()
    {
        auto owned = std::make_unique<MockProcessProbe>();
        probe = owned.get();
        model = std::make_unique<Domain::ProcessModel>(std::move(owned), [this] { return currentTime; });
    }

    // Advances the clock by @p elapsed (none for the first sample), then samples one process
    // whose open connections have sent and received the given byte totals.
    // @p netSampleTimeNs: when the probe read the network counters (0 = with this refresh).
    auto sample(std::chrono::milliseconds elapsed, std::uint64_t sent, std::uint64_t received, std::uint64_t netSampleTimeNs = 0)
        -> Domain::ProcessSnapshot
    {
        currentTime += elapsed;
        totalCpuTime += 100000;
        probe->setCounters({});
        probe->withProcess(100, "network_proc").withNetworkCounters(100, sent, received).withNetworkSampleTime(100, netSampleTimeNs);
        probe->setTotalCpuTime(totalCpuTime);
        model->refresh();
        const auto snaps = model->snapshots();
        EXPECT_EQ(snaps.size(), 1U);
        return snaps.empty() ? Domain::ProcessSnapshot{} : snaps.front();
    }
};

} // namespace

TEST(ProcessModelTest, NetworkRatesCalculatedFromDeltas)
{
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 1000, 2000);

    const auto snap = fixture.sample(std::chrono::milliseconds{500}, 2000, 4000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 2000.0);     // 1000 bytes / 0.5 s
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 4000.0); // 2000 bytes / 0.5 s
}

TEST(ProcessModelTest, NetworkRatesFollowTheLastIntervalNotTheLifetimeAverage)
{
    // #1036: the rate was (bytes now - bytes when first seen) / time since first seen, so a
    // burst decayed slowly instead of dropping to zero when the transfer stopped.
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 0, 0);

    auto snap = fixture.sample(std::chrono::seconds{1}, 1'000'000, 3'000'000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 1'000'000.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 3'000'000.0);

    // Transfer stopped: the lifetime average would still read 500 KB/s and 1.5 MB/s here.
    snap = fixture.sample(std::chrono::seconds{1}, 1'000'000, 3'000'000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);

    // A later, smaller transfer shows at its own rate, not diluted by the time already watched.
    snap = fixture.sample(std::chrono::seconds{1}, 1'010'000, 3'020'000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 10'000.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 20'000.0);
}

TEST(ProcessModelTest, NetworkRatesHandleCounterDecrease)
{
    // The counters are sums over live connections; one closing makes the sum drop. That interval
    // reads 0, not a wrapped (huge) or negative rate, and the next interval is measured from the
    // lower sum.
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 2000, 4000);

    auto snap = fixture.sample(std::chrono::seconds{1}, 500, 1000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);

    snap = fixture.sample(std::chrono::seconds{1}, 1500, 1500);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 1000.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 500.0);
}

TEST(ProcessModelTest, NetworkRatesZeroForImplausiblyShortInterval)
{
    // An interval under half the minimum refresh interval (the seed refresh followed at once by
    // the sampler's first) is treated as no previous data, so it cannot divide by a tiny time.
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 1000, 2000);

    auto snap = fixture.sample(std::chrono::milliseconds{1}, 5000, 10000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);

    // From a nonzero rate (#1063 review): a too-short interval resets the rate to 0, not the last
    // rate republished -- only a repeated cached read from a probe that stamps its reads holds it.
    snap = fixture.sample(std::chrono::seconds{1}, 6000, 12000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 1000.0);
    snap = fixture.sample(std::chrono::milliseconds{1}, 6000, 12000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);
}

TEST(ProcessModelTest, NetworkRatesUseTheProbeReadIntervalWhenItCaches)
{
    // #1063 review: the Linux probe caches its socket query for 500 ms. With 100 ms refreshes a steady
    // 1 MB/s transfer read as four refreshes of 0 and then one of 5 MB/s when the rate was taken over
    // the refresh interval. Over the time between the probe's reads, it is 1 MB/s throughout.
    constexpr std::uint64_t MS = 1'000'000; // ns
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 0, 0, 1000 * MS);

    constexpr auto REFRESH = std::chrono::milliseconds{100};
    auto snap = fixture.sample(REFRESH, 0, 0, 1000 * MS); // still the cached read
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);

    // Fresh read 500 ms after the first: 500 KB over 0.5 s.
    snap = fixture.sample(REFRESH, 0, 500'000, 1500 * MS);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000'000.0);

    // Cached refreshes after it hold the rate rather than reading 0.
    for (int i = 0; i < 4; ++i)
    {
        snap = fixture.sample(REFRESH, 0, 500'000, 1500 * MS);
        EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000'000.0);
    }

    // The next fresh read: another 500 KB over 0.5 s, not 5x the rate.
    snap = fixture.sample(REFRESH, 0, 1'000'000, 2000 * MS);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000'000.0);

    // Transfer stopped: the next fresh read drops it to 0.
    snap = fixture.sample(REFRESH, 0, 1'000'000, 2500 * MS);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 0.0);
}

TEST(ProcessModelTest, NetworkRatesAboveSanityCeilingAreDropped)
{
    // A connection that appears carrying traffic from before it was first seen can add far more
    // bytes than one interval could carry; such a rate is reported as 0, not as a spike.
    NetworkRateFixture fixture;
    fixture.sample(std::chrono::milliseconds{0}, 0, 0);

    const auto huge = static_cast<std::uint64_t>(Domain::Sampling::MAX_SANE_RATE_BPS_DEFAULT) * 2U;
    const auto snap = fixture.sample(std::chrono::seconds{1}, huge, 1000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1000.0);
}

TEST(ProcessModelTest, ConfiguredSanityCeilingRaisesTheLimit)
{
    // [metrics] max_sane_rate_bps (#1123): raised for a fast link, a rate above the default 100 Gbps
    // ceiling but below the configured one is reported, not dropped.
    NetworkRateFixture fixture;
    fixture.model->setMaxSaneNetworkRate(Domain::Sampling::MAX_SANE_RATE_BPS_MAX);
    fixture.sample(std::chrono::milliseconds{0}, 0, 0);

    const auto aboveDefault = static_cast<std::uint64_t>(Domain::Sampling::MAX_SANE_RATE_BPS_DEFAULT) * 2U;
    const auto snap = fixture.sample(std::chrono::seconds{1}, aboveDefault, 0);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, static_cast<double>(aboveDefault));
}

TEST(ProcessModelTest, ConfiguredSanityCeilingLowersTheLimit)
{
    NetworkRateFixture fixture;
    fixture.model->setMaxSaneNetworkRate(Domain::Sampling::MAX_SANE_RATE_BPS_MIN);
    fixture.sample(std::chrono::milliseconds{0}, 0, 0);

    const auto aboveMin = static_cast<std::uint64_t>(Domain::Sampling::MAX_SANE_RATE_BPS_MIN) * 2U;
    const auto snap = fixture.sample(std::chrono::seconds{1}, aboveMin, 1000);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1000.0);
}

TEST(ProcessModelTest, ConfiguredSanityCeilingIsClamped)
{
    // Out-of-range values (a NaN, a ceiling of 0) take the nearest bound rather than disabling
    // network rates or the check.
    NetworkRateFixture fixture;
    fixture.model->setMaxSaneNetworkRate(0.0);
    fixture.sample(std::chrono::milliseconds{0}, 0, 0);

    const auto belowMin = static_cast<std::uint64_t>(Domain::Sampling::MAX_SANE_RATE_BPS_MIN) / 2U;
    auto snap = fixture.sample(std::chrono::seconds{1}, belowMin, 0);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, static_cast<double>(belowMin));

    fixture.model->setMaxSaneNetworkRate(std::numeric_limits<double>::quiet_NaN());
    snap = fixture.sample(std::chrono::seconds{1}, belowMin * 2U, 0);
    EXPECT_DOUBLE_EQ(snap.netSentBytesPerSec, static_cast<double>(belowMin));
}

namespace
{

// Drives a ProcessModel whose probe reports per-connection readings (readSocketTraffic(), as the
// Linux probe does) instead of per-process network counters, so the model accumulates them (#1099).
struct SocketTrafficFixture
{
    static constexpr std::uint64_t MS = 1'000'000; // ns
    static constexpr std::int32_t PID = 100;

    Domain::ProcessModel::Clock::time_point currentTime;
    MockProcessProbe* probe = nullptr;
    std::unique_ptr<Domain::ProcessModel> model;
    std::uint64_t totalCpuTime = 100000;

    SocketTrafficFixture()
    {
        auto owned = std::make_unique<MockProcessProbe>();
        probe = owned.get();
        probe->withProcess(PID, "network_proc");
        model = std::make_unique<Domain::ProcessModel>(std::move(owned), [this] { return currentTime; });
    }

    // Advances the clock by one second, then refreshes with `reading` as the probe's socket reading.
    auto sample(Platform::SocketTrafficReading reading) -> Domain::ProcessSnapshot
    {
        currentTime += std::chrono::seconds{1};
        totalCpuTime += 100000;
        probe->setTotalCpuTime(totalCpuTime);
        probe->setSocketTraffic(std::move(reading));
        model->refresh();
        const auto snaps = model->snapshots();
        EXPECT_EQ(snaps.size(), 1U);
        return snaps.empty() ? Domain::ProcessSnapshot{} : snaps.front();
    }
};

} // namespace

TEST(ProcessModelTest, AClosingSocketDoesNotZeroTheProcessNetworkRate)
{
    // #1099: the per-process counter was the sum over the process's live sockets, so when socket 12
    // closed it dropped from 6000 to 3000 and the process read 0 B/s for the interval in which
    // socket 11 moved 2000 bytes. Accumulated from per-socket deltas it only ever grows.
    constexpr std::uint64_t MS = SocketTrafficFixture::MS;
    constexpr std::int32_t PID = SocketTrafficFixture::PID;
    SocketTrafficFixture fixture;
    fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 1'000}, {.key = 12, .pid = PID, .bytesReceived = 5'000}},
                    .sampleTimeNs = 1000 * MS});

    auto snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 3'000}}, .sampleTimeNs = 2000 * MS}); // 12 closed
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 2'000.0);

    snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 6'000}}, .sampleTimeNs = 3000 * MS});
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 3'000.0);
}

TEST(ProcessModelTest, AnOlderSocketReadingIsNotFolded)
{
    // A reading older than the last one folded must not rewind the socket baselines: folded, socket
    // 11 back at 1000 and then at 4000 again would count the 2000 bytes from 1000 to 3000 twice.
    constexpr std::uint64_t MS = SocketTrafficFixture::MS;
    constexpr std::int32_t PID = SocketTrafficFixture::PID;
    SocketTrafficFixture fixture;
    fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 1'000}}, .sampleTimeNs = 1000 * MS});
    auto snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 3'000}}, .sampleTimeNs = 2000 * MS});
    ASSERT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 2'000.0);

    snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 1'000}}, .sampleTimeNs = 1500 * MS});
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 2'000.0) << "the stale reading holds the last rate";

    snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 4'000}}, .sampleTimeNs = 3000 * MS});
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000.0) << "1000 bytes over the second since the last folded reading";
}

TEST(ProcessModelTest, AFailedSocketReadingHoldsTheLastNetworkRate)
{
    // A failed reading (sampleTimeNs 0) republishes the last totals and their time: the rate holds,
    // and the next reading is measured over the time since the last good one, not as a burst.
    constexpr std::uint64_t MS = SocketTrafficFixture::MS;
    constexpr std::int32_t PID = SocketTrafficFixture::PID;
    SocketTrafficFixture fixture;
    fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 0}}, .sampleTimeNs = 1000 * MS});
    auto snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 1'000}}, .sampleTimeNs = 2000 * MS});
    ASSERT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000.0);

    snap = fixture.sample({});
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000.0);

    snap = fixture.sample({.sockets = {{.key = 11, .pid = PID, .bytesReceived = 3'000}}, .sampleTimeNs = 4000 * MS});
    EXPECT_DOUBLE_EQ(snap.netReceivedBytesPerSec, 1'000.0); // 2000 bytes over the 2 s since the last good reading
}

// =============================================================================
// Power Usage Calculation Tests
// =============================================================================

TEST(ProcessModelTest, FirstRefreshShowsZeroPowerUsage)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(100, "test_proc").withPowerUsage(100, 1'000'000); // 1M microjoules
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // First sample has no previous data, so power should be 0
    EXPECT_DOUBLE_EQ(snaps[0].powerWatts, 0.0);
}

TEST(ProcessModelTest, PowerUsageCalculationFromEnergyDelta)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First refresh: energy = 1,000,000 microjoules (1 joule)
    rawProbe->withProcess(100, "power_proc").withPowerUsage(100, 1'000'000);
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(100));

    // Second refresh: energy increased by 100,000 microjoules (0.1 joule)
    // If 100ms passed, power = 0.1J / 0.1s = 1W
    rawProbe->withProcess(100, "power_proc").withPowerUsage(100, 1'100'000);
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // 0.1 J over exactly 0.1 s
    EXPECT_DOUBLE_EQ(snaps[0].powerWatts, 1.0);
}

// #1093: a probe that reports a package energy counter (Linux RAPL) has it shared out per interval
// by each process's CPU time in that interval, under ProcessModel's sampling lock.
TEST(ProcessModelTest, PackageEnergyIsSharedByIntervalCpuTime)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withProcess(1, "busy").withCpuTime(1, 100, 0);
    rawProbe->withProcess(2, "idle_daemon").withCpuTime(2, 1'000'000, 0); // lots of lifetime CPU
    rawProbe->setTotalCpuTime(10'000);
    rawProbe->setPackageEnergy(Platform::PackageEnergyReading{.energyUj = 5'000'000, .maxRangeUj = 0, .busyCpuTicks = std::nullopt});

    Domain::ProcessModel::Clock::time_point now{};
    Domain::ProcessModel model(std::move(probe), [&now] { return now; });
    model.refresh();

    // One second later: the busy process used all the CPU, and the package used 2 J.
    now += std::chrono::seconds(1);
    rawProbe->withCpuTime(1, 200, 0);
    rawProbe->setTotalCpuTime(10'100);
    rawProbe->setPackageEnergy(Platform::PackageEnergyReading{.energyUj = 7'000'000, .maxRangeUj = 0, .busyCpuTicks = std::nullopt});
    model.refresh();

    const auto snaps = model.snapshots();
    const auto busy = std::ranges::find_if(snaps, [](const Domain::ProcessSnapshot& s) { return s.pid == 1; });
    const auto idle = std::ranges::find_if(snaps, [](const Domain::ProcessSnapshot& s) { return s.pid == 2; });
    ASSERT_NE(busy, snaps.end());
    ASSERT_NE(idle, snaps.end());
    EXPECT_DOUBLE_EQ(busy->powerWatts, 2.0);
    EXPECT_DOUBLE_EQ(idle->powerWatts, 0.0);
}

TEST(ProcessModelTest, PowerUsageWithZeroEnergyDelta)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // Process with constant energy (no power consumption)
    rawProbe->withProcess(100, "idle_proc").withPowerUsage(100, 1'000'000);
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

    // Energy unchanged
    rawProbe->withProcess(100, "idle_proc").withPowerUsage(100, 1'000'000);
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].powerWatts, 0.0);
}

TEST(ProcessModelTest, PowerUsageHandlesEnergyCounterReset)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First reading
    rawProbe->withProcess(100, "proc").withPowerUsage(100, 5'000'000);
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

    // Counter decreased (reset or wrap) - should be handled gracefully
    rawProbe->withProcess(100, "proc").withPowerUsage(100, 1'000'000);
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // When energy counter decreases, no power is calculated (0 or skipped)
    EXPECT_DOUBLE_EQ(snaps[0].powerWatts, 0.0);
}

TEST(ProcessModelTest, PowerUsageWithoutEnergyData)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->withProcess(100, "no_power_proc");
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

    rawProbe->withProcess(100, "no_power_proc");
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].powerWatts, 0.0);
}

TEST(ProcessModelTest, BuilderPatternWithPowerUsage)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(123, "power_test").withPowerUsage(123, 5'000'000).withCpuTime(123, 1000, 500);
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].pid, 123);
    EXPECT_EQ(snaps[0].name, "power_test");
}

// =============================================================================
// I/O Rate Calculation Tests
// =============================================================================
TEST(ProcessModelTest, FirstRefreshShowsZeroIoRates)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters c = makeCounter(100, "test_proc", 'R', 1000, 500);
    c.readBytes = 1024 * 1024; // 1 MB
    c.writeBytes = 512 * 1024; // 512 KB

    probe->setCounters({c});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 0.0); // No previous data
    EXPECT_DOUBLE_EQ(snaps[0].ioWriteBytesPerSec, 0.0);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(ProcessModelTest, EmptyCountersResultInEmptySnapshots)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.processCount(), 0);
    EXPECT_TRUE(model.snapshots().empty());
}

TEST(ProcessModelTest, LargeNumberOfProcesses)
{
    auto probe = std::make_unique<MockProcessProbe>();

    std::vector<Platform::ProcessCounters> counters;
    for (int32_t i = 0; i < 1000; ++i)
    {
        counters.push_back(
            makeCounter(i + 1, "proc_" + std::to_string(i), 'S', static_cast<uint64_t>(i) * 100, static_cast<uint64_t>(i) * 50));
    }
    probe->setCounters(counters);
    probe->setTotalCpuTime(10000000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.processCount(), 1000);
}

TEST(ProcessModelTest, ProcessWithZeroStartTime)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "kernel_thread", 'S', 1000, 500, /*startTime*/ 0)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // Should still work - uniqueKey based on hash of 0 is valid
    EXPECT_NE(snaps[0].uniqueKey, 0);
}

TEST(ProcessModelTest, IntegerOverflowInCpuCounters)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // Start with very high values near overflow
    constexpr uint64_t nearMax = std::numeric_limits<uint64_t>::max() - OVERFLOW_TEST_MARGIN;
    rawProbe->setCounters({makeCounter(100, "overflow_proc", 'R', nearMax, 5000)});
    rawProbe->setTotalCpuTime(nearMax * 2);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Counter wraps around (overflow scenario)
    // In practice, OS counters may wrap, but our delta calculation should handle it gracefully
    // by treating the new value as a new baseline
    rawProbe->setCounters({makeCounter(100, "overflow_proc", 'R', 1000, 500)});
    rawProbe->setTotalCpuTime(nearMax * 2 + 100000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // CPU% should be 0 or minimal because the counter appears to have decreased
    // (which our implementation treats as a new process baseline)
    EXPECT_GE(snaps[0].cpuPercent, 0.0);
    // CPU% is calculated as (processDelta / totalCpuDelta) * 100, so it should be <= 100%
    // regardless of core count (totalCpuDelta includes all cores)
    EXPECT_LE(snaps[0].cpuPercent, 100.0);
}

TEST(ProcessModelTest, ExtremeValuesMaxUint64)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Platform::ProcessCounters c;
    c.pid = std::numeric_limits<int32_t>::max();
    c.parentPid = std::numeric_limits<int32_t>::max() - 1;
    c.name = "extreme_proc";
    c.state = 'R';
    c.userTime = std::numeric_limits<uint64_t>::max();
    c.systemTime = std::numeric_limits<uint64_t>::max();
    c.startTimeTicks = std::numeric_limits<uint64_t>::max();
    c.rssBytes = std::numeric_limits<uint64_t>::max();
    c.virtualBytes = std::numeric_limits<uint64_t>::max();
    c.threadCount = std::numeric_limits<int32_t>::max();

    probe->setCounters({c});
    probe->setTotalCpuTime(std::numeric_limits<uint64_t>::max());

    Domain::ProcessModel model(std::move(probe));

    // Should not crash or produce undefined behavior
    EXPECT_NO_THROW({ model.refresh(); });

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Verify extreme values are preserved
    EXPECT_EQ(snaps[0].pid, std::numeric_limits<int32_t>::max());
    EXPECT_EQ(snaps[0].parentPid, std::numeric_limits<int32_t>::max() - 1);
    EXPECT_EQ(snaps[0].name, "extreme_proc");
    EXPECT_EQ(snaps[0].memoryBytes, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(snaps[0].virtualBytes, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(snaps[0].threadCount, std::numeric_limits<int32_t>::max());

    // CPU% should be valid (0.0 on first sample, no previous data)
    EXPECT_GE(snaps[0].cpuPercent, 0.0);
    EXPECT_LE(snaps[0].cpuPercent, 100.0);

    // UniqueKey should be valid (non-zero hash)
    EXPECT_NE(snaps[0].uniqueKey, 0);
}

// =============================================================================
// Builder Pattern Tests
// =============================================================================

TEST(ProcessModelTest, BuilderPatternSimpleSetup)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(123, "test_process").withCpuTime(123, 1000, 500).withMemory(123, 4096 * 1024).withState(123, 'R');
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].pid, 123);
    EXPECT_EQ(snaps[0].name, "test_process");
    EXPECT_EQ(snaps[0].displayState, "Running");
    EXPECT_EQ(snaps[0].memoryBytes, 4096 * 1024);
}

TEST(ProcessModelTest, BuilderPatternMultipleProcesses)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(100, "proc_a")
        .withState(100, 'R')
        .withProcess(200, "proc_b")
        .withState(200, 'S')
        .withProcess(300, "proc_c")
        .withState(300, 'D');
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    EXPECT_EQ(model.processCount(), 3);
}

TEST(ProcessModelTest, BuilderPatternBackwardCompatibility)
{
    // Old style still works
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(123, "legacy_proc", 'R', 1000, 500)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].pid, 123);
    EXPECT_EQ(snaps[0].name, "legacy_proc");
}

TEST(ProcessModelTest, IoRatesCalculatedFromDeltas)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First sample: process has read 1 MB, written 512 KB
    Platform::ProcessCounters c1 = makeCounter(100, "test_proc", 'R', 1000, 500);
    c1.readBytes = 1024 * 1024;
    c1.writeBytes = 512 * 1024;

    rawProbe->setCounters({c1});
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(100));

    // Second sample: process has read 3 MB total (delta = 2 MB), written 1.5 MB total (delta = 1 MB)
    Platform::ProcessCounters c2 = makeCounter(100, "test_proc", 'R', 2000, 1000);
    c2.readBytes = 3 * 1024 * 1024;
    c2.writeBytes = 1536 * 1024;

    rawProbe->setCounters({c2});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Read delta = 2 MB, write delta = 1 MB, over exactly 0.1 s
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 20.0 * 1024.0 * 1024.0);
    EXPECT_DOUBLE_EQ(snaps[0].ioWriteBytesPerSec, 10.0 * 1024.0 * 1024.0);
}

TEST(ProcessModelTest, IoRatesHandleNoActivity)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    Platform::ProcessCounters c1 = makeCounter(100, "idle_proc", 'S', 1000, 500);
    c1.readBytes = 1024 * 1024;
    c1.writeBytes = 512 * 1024;

    rawProbe->setCounters({c1});
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

    // Second sample: no change in I/O counters
    Platform::ProcessCounters c2 = makeCounter(100, "idle_proc", 'S', 1000, 500);
    c2.readBytes = 1024 * 1024; // Same as before
    c2.writeBytes = 512 * 1024; // Same as before

    rawProbe->setCounters({c2});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snaps[0].ioWriteBytesPerSec, 0.0);
}

TEST(ProcessModelTest, IoRatesForMultipleProcesses)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First sample: two processes
    Platform::ProcessCounters c1a = makeCounter(100, "proc_a", 'R', 1000, 0);
    c1a.readBytes = 1024 * 1024;
    c1a.writeBytes = 512 * 1024;

    Platform::ProcessCounters c1b = makeCounter(200, "proc_b", 'R', 2000, 0);
    c1b.readBytes = 2 * 1024 * 1024;
    c1b.writeBytes = 1024 * 1024;

    rawProbe->setCounters({c1a, c1b});
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(100));

    // Second sample: proc_a read 1 MB more, proc_b wrote 2 MB more
    Platform::ProcessCounters c2a = makeCounter(100, "proc_a", 'R', 1500, 0);
    c2a.readBytes = 2 * 1024 * 1024; // +1 MB
    c2a.writeBytes = 512 * 1024;     // No change

    Platform::ProcessCounters c2b = makeCounter(200, "proc_b", 'R', 3000, 0);
    c2b.readBytes = 2 * 1024 * 1024;  // No change
    c2b.writeBytes = 3 * 1024 * 1024; // +2 MB

    rawProbe->setCounters({c2a, c2b});
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

    // proc_a read 1 MB in 0.1 s, wrote nothing
    EXPECT_DOUBLE_EQ(snapA->ioReadBytesPerSec, 10.0 * 1024.0 * 1024.0);
    EXPECT_DOUBLE_EQ(snapA->ioWriteBytesPerSec, 0.0);

    // proc_b wrote 2 MB in 0.1 s, read nothing
    EXPECT_DOUBLE_EQ(snapB->ioReadBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snapB->ioWriteBytesPerSec, 20.0 * 1024.0 * 1024.0);
}

TEST(ProcessModelTest, IoRatesHandleCounterWrapAround)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // First sample with high counter values
    Platform::ProcessCounters c1 = makeCounter(100, "wrap_proc", 'R', 1000, 500);
    c1.readBytes = 1000 * 1024 * 1024; // 1000 MB
    c1.writeBytes = 500 * 1024 * 1024; // 500 MB

    rawProbe->setCounters({c1});
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

    // Second sample: counter appears to have decreased (wraparound or reset)
    // Our implementation should handle this gracefully by showing 0 rate
    Platform::ProcessCounters c2 = makeCounter(100, "wrap_proc", 'R', 2000, 1000);
    c2.readBytes = 100 * 1024 * 1024; // Less than before
    c2.writeBytes = 50 * 1024 * 1024; // Less than before

    rawProbe->setCounters({c2});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Should handle gracefully (no negative rates)
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(snaps[0].ioWriteBytesPerSec, 0.0);
}

TEST(ProcessModelTest, NewProcessWithSamePidGetsZeroIoRates)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    // Original process PID 100, startTime 1000
    Platform::ProcessCounters c1 = makeCounter(100, "original", 'R', 10000, 5000, /*startTime*/ 1000);
    c1.readBytes = 1024 * 1024;
    c1.writeBytes = 512 * 1024;

    rawProbe->setCounters({c1});
    rawProbe->setTotalCpuTime(100000);

    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::milliseconds(50));

    // New process reuses PID 100 but has different startTime
    Platform::ProcessCounters c2 = makeCounter(100, "new_proc", 'R', 100, 50, /*startTime*/ 2000);
    c2.readBytes = 2 * 1024 * 1024;
    c2.writeBytes = 1024 * 1024;

    rawProbe->setCounters({c2});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].name, "new_proc");
    EXPECT_DOUBLE_EQ(snaps[0].ioReadBytesPerSec, 0.0); // No valid previous data
    EXPECT_DOUBLE_EQ(snaps[0].ioWriteBytesPerSec, 0.0);
}

// =============================================================================
// Handle Count Pass-Through Tests
// =============================================================================

TEST(ProcessModelTest, HandleCountIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "handles_test", 'R', 1000, 500);
    counter.handleCount = 42;
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].handleCount, 42);
}

TEST(ProcessModelTest, HandleCountZeroIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "no_handles", 'R', 1000, 500);
    counter.handleCount = 0;
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].handleCount, 0);
}

// =============================================================================
// Start Time Epoch Pass-Through Tests
// =============================================================================

TEST(ProcessModelTest, StartTimeEpochIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "epoch_test", 'R', 1000, 500);
    counter.startTimeEpoch = 1704067200; // 2024-01-01 00:00:00 UTC
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].startTimeEpoch, 1704067200);
}

TEST(ProcessModelTest, StartTimeEpochZeroIsPassedThrough)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "no_epoch", 'R', 1000, 500);
    counter.startTimeEpoch = 0; // Unknown/unavailable
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].startTimeEpoch, 0);
}

TEST(ProcessModelTest, StartTimeTicksArePassedThrough)
{
    // The raw start time is what process actions verify before acting (#973). If the model ever
    // stopped copying it, every action from the UI would carry an unknown identity and be refused,
    // and neither the dispatch tests (hand-built snapshots) nor the platform contract tests
    // (which bypass the model) would notice.
    auto probe = std::make_unique<MockProcessProbe>();
    constexpr uint64_t START_TICKS = 133'987'654'321'000'000ULL; // A FILETIME-sized value
    probe->setCounters({makeCounter(100, "ticks_test", 'R', 1000, 500, START_TICKS)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].startTimeTicks, START_TICKS);
}

// =============================================================================
// System-Level History Tests (for untested functions)
// =============================================================================

TEST(ProcessModelTest, SystemNetSentHistoryIsEmptyInitially)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    auto history = model.systemNetSentHistory();
    EXPECT_TRUE(history.empty());
}

TEST(ProcessModelTest, SystemNetRecvHistoryIsEmptyInitially)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    auto history = model.systemNetRecvHistory();
    EXPECT_TRUE(history.empty());
}

TEST(ProcessModelTest, SystemPageFaultsHistoryIsEmptyInitially)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    auto history = model.systemPageFaultsHistory();
    EXPECT_TRUE(history.empty());
}

TEST(ProcessModelTest, SystemThreadCountHistoryIsEmptyInitially)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    auto history = model.systemThreadCountHistory();
    EXPECT_TRUE(history.empty());
}

TEST(ProcessModelTest, SystemHandleCountHistoryIsEmptyInitially)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    auto history = model.systemHandleCountHistory();
    EXPECT_TRUE(history.empty());
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

    const auto history = model.systemHandleCountHistory();
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

    const auto timestamps = model.historyTimestamps();
    const auto handleHistory = model.systemHandleCountHistory();
    EXPECT_EQ(timestamps.size(), handleHistory.size());
}

TEST(ProcessModelTest, SystemPowerHistoryIsEmptyInitially)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    auto history = model.systemPowerHistory();
    EXPECT_TRUE(history.empty());
}

TEST(ProcessModelTest, HistoryTimestampsAreEmptyInitially)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::ProcessModel model(std::move(probe));

    auto timestamps = model.historyTimestamps();
    EXPECT_TRUE(timestamps.empty());
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
    ASSERT_EQ(model.historyTimestamps().size(), 5U);

    // Clamped to HISTORY_SECONDS_MIN like every other model (#1145), where it used to keep a
    // zero-second window and with it only the current sample: t = 15, 20, 25, plus t = 10 kept just
    // before the cutoff (#1016).
    static_assert(Domain::Sampling::HISTORY_SECONDS_MIN == 10, "the expected count below assumes a 10 s minimum");
    model.setMaxHistorySeconds(0.0);
    EXPECT_EQ(model.historyTimestamps().size(), 4U);
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

// =============================================================================
// Peak RSS Tracking Tests
// =============================================================================

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

    gpuModel->refresh();
    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1);

    // Memory should be summed across GPUs
    EXPECT_EQ(snaps[0].gpuMemoryBytes, (256 + 512) * 1024ULL * 1024);
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

    gpuModel->refresh();
    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // gpuDevices should contain the GPU name
    EXPECT_FALSE(snaps[0].gpuDevices.empty());
}

TEST(ProcessModelTest, InteractionModeReusesCachedGpuDataBetweenMerges)
{
    auto currentTime = Domain::ProcessModel::Clock::time_point{};
    auto processProbe = std::make_unique<MockProcessProbe>();
    auto* rawProcessProbe = processProbe.get();
    rawProcessProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1000, 500)});
    rawProcessProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    auto* rawGpuProbe = gpuProbe.get();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Test GPU", "TestVendor").withProcessGPU(100, "GPU0", 512ULL * 1024 * 1024);
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));

    Domain::ProcessModel processModel(std::move(processProbe), [&currentTime] { return currentTime; });
    processModel.setGPUModel(gpuModel);
    gpuModel->refresh();
    processModel.refresh();
    const auto initialProcessGpuQueryCount = rawGpuProbe->readProcessCountersCallCount();
    ASSERT_EQ(initialProcessGpuQueryCount, 1);

    processModel.setInteractionActive(true);
    currentTime += std::chrono::seconds(1);
    rawProcessProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1100, 500)});
    rawProcessProbe->setTotalCpuTime(200000);
    processModel.refresh();

    const auto snapshots = processModel.snapshots();
    ASSERT_EQ(snapshots.size(), 1);
    EXPECT_EQ(snapshots[0].gpuMemoryBytes, 512ULL * 1024 * 1024);
    EXPECT_EQ(snapshots[0].gpuDevices, "Test GPU");
    EXPECT_EQ(rawGpuProbe->readProcessCountersCallCount(), initialProcessGpuQueryCount);

    currentTime += std::chrono::milliseconds(500);
    rawProcessProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1200, 500)});
    rawProcessProbe->setTotalCpuTime(300000);
    processModel.refresh();

    EXPECT_EQ(rawGpuProbe->readProcessCountersCallCount(), initialProcessGpuQueryCount + 1);
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

    gpuModel->refresh();
    processModel.refresh();

    auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    // Should aggregate memory from both GPUs
    EXPECT_EQ(snaps[0].gpuMemoryBytes, 768ULL * 1024 * 1024); // 512 + 256 MB
    // Should include both GPU names
    EXPECT_FALSE(snaps[0].gpuDevices.empty());
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

// =============================================================================
// Publisher, Type, and GDI Object Passthrough Tests (Issues #184, #185, #195)
// =============================================================================

TEST(ProcessModelTest, WhenProbeReturnsPublisher_ThenSnapshotContainsPublisher)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "app", 'R', 1000, 500);
    counter.publisher = "Test Corporation";
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].publisher, "Test Corporation");
}

TEST(ProcessModelTest, WhenProbeReturnsEmptyPublisher_ThenSnapshotPublisherIsEmpty)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "app", 'R', 1000, 500);
    counter.publisher = "";
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_TRUE(snaps[0].publisher.empty());
}

TEST(ProcessModelTest, WhenProbeReturnsProcessType_ThenSnapshotContainsProcessType)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "app", 'R', 1000, 500);
    counter.processType = "App";
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].processType, "App");
}

TEST(ProcessModelTest, WhenProbeReturnsBackgroundProcessType_ThenSnapshotContainsIt)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "svc", 'R', 1000, 500);
    counter.processType = "Background Process";
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    EXPECT_EQ(snaps[0].processType, "Background Process");
}

TEST(ProcessModelTest, WhenProbeReturnsGdiObjectCount_ThenSnapshotContainsCount)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "app", 'R', 1000, 500);
    counter.gdiObjectCount = std::int32_t{42};
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    ASSERT_TRUE(snaps[0].gdiObjectCount.has_value());
    EXPECT_EQ(*snaps[0].gdiObjectCount, 42);
}

TEST(ProcessModelTest, WhenProbeReturnsZeroGdiObjectCount_ThenSnapshotCountIsZero)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCounters counter = makeCounter(100, "proc", 'R', 1000, 500);
    counter.gdiObjectCount = std::int32_t{0};
    probe->setCounters({counter});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 1);
    ASSERT_TRUE(snaps[0].gdiObjectCount.has_value());
    EXPECT_EQ(*snaps[0].gdiObjectCount, 0);
}

TEST(ProcessModelTest, ProcessTreeHierarchyIsCorrectlyBuilt)
{
    auto probe = std::make_unique<MockProcessProbe>();
    // proc_1 (pid=1, parentPid=0) -> root
    // proc_2 (pid=2, parentPid=1) -> child of proc_1
    // proc_3 (pid=3, parentPid=2) -> child of proc_2
    // proc_4 (pid=4, parentPid=1) -> child of proc_1
    // proc_5 (pid=5, parentPid=999) -> missing parent, acts as root
    probe->setCounters({makeCounter(1, "proc_1", 'R', 1000, 500, 1000, 1024, 0),
                        makeCounter(2, "proc_2", 'R', 1000, 500, 1000, 1024, 1),
                        makeCounter(3, "proc_3", 'R', 1000, 500, 1000, 1024, 2),
                        makeCounter(4, "proc_4", 'R', 1000, 500, 1000, 1024, 1),
                        makeCounter(5, "proc_5", 'R', 1000, 500, 1000, 1024, 999)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 5);

    // Verify proc_1 has children (proc_2 and proc_4)
    EXPECT_EQ(snaps[0].pid, 1);
    EXPECT_EQ(snaps[0].childrenIndices.size(), 2);
    EXPECT_TRUE(std::find(snaps[0].childrenIndices.begin(), snaps[0].childrenIndices.end(), 1) !=
                snaps[0].childrenIndices.end()); // proc_2 is at index 1
    EXPECT_TRUE(std::find(snaps[0].childrenIndices.begin(), snaps[0].childrenIndices.end(), 3) !=
                snaps[0].childrenIndices.end()); // proc_4 is at index 3

    // Verify proc_2 has child (proc_3)
    EXPECT_EQ(snaps[1].pid, 2);
    EXPECT_EQ(snaps[1].childrenIndices.size(), 1);
    EXPECT_EQ(snaps[1].childrenIndices[0], 2); // proc_3 is at index 2

    // Verify proc_3, proc_4, proc_5 have no children
    EXPECT_EQ(snaps[2].pid, 3);
    EXPECT_TRUE(snaps[2].childrenIndices.empty());

    EXPECT_EQ(snaps[3].pid, 4);
    EXPECT_TRUE(snaps[3].childrenIndices.empty());

    EXPECT_EQ(snaps[4].pid, 5);
    EXPECT_TRUE(snaps[4].childrenIndices.empty());
}

TEST(ProcessModelTest, ProcessTreeHandlesPidReuseCorrectly)
{
    auto probe = std::make_unique<MockProcessProbe>();
    // A parent process dies, and its PID is reused by a completely unrelated process within the
    // same sampling interval. Child processes still pointing to the old parent PID must NOT be
    // linked to the new process, since that would misattach them to an unrelated process (#595).

    // pid=1 is the *new* process (start time 2000, i.e. it started after this snapshot's older
    // processes). proc_2 (start time 1000) reports parentPid=1, but since a real parent must
    // start at or before its child, pid=1's current holder cannot be proc_2's actual parent --
    // that was a different, already-exited process that used to hold PID 1.
    probe->setCounters(
        {makeCounter(1, "new_proc", 'R', 1000, 500, 2000, 1024, 0), makeCounter(2, "child_proc", 'R', 1000, 500, 1000, 1024, 1)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);

    // new_proc must NOT have child_proc linked as a child -- the start-time ordering rules
    // out pid=1's current holder as the real parent, so child_proc is treated as unparented.
    EXPECT_EQ(snaps[0].pid, 1);
    EXPECT_TRUE(snaps[0].childrenIndices.empty());
}

TEST(ProcessModelTest, ProcessTreeLinksChildWhenParentStartedFirst)
{
    auto probe = std::make_unique<MockProcessProbe>();
    // Normal (non-reuse) case: parent genuinely started before its child, so the start-time
    // guard added for #595 must not reject a legitimate parent/child link.
    probe->setCounters(
        {makeCounter(1, "parent_proc", 'R', 1000, 500, 1000, 1024, 0), makeCounter(2, "child_proc", 'R', 1000, 500, 2000, 1024, 1)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);

    EXPECT_EQ(snaps[0].pid, 1);
    ASSERT_EQ(snaps[0].childrenIndices.size(), 1);
    EXPECT_EQ(snaps[0].childrenIndices[0], 1);
}

TEST(ProcessModelTest, ProcessTreeLinksChildWhenStartTimesUnavailable)
{
    auto probe = std::make_unique<MockProcessProbe>();
    // When a probe can't report start times (startTimeTicks == 0 for both), the guard has no
    // data to validate against and must fall back to linking by PID alone rather than silently
    // dropping every parent/child relationship.
    probe->setCounters(
        {makeCounter(1, "parent_proc", 'R', 1000, 500, 0, 1024, 0), makeCounter(2, "child_proc", 'R', 1000, 500, 0, 1024, 1)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    const auto snaps = model.snapshots();
    ASSERT_EQ(snaps.size(), 2);

    EXPECT_EQ(snaps[0].pid, 1);
    ASSERT_EQ(snaps[0].childrenIndices.size(), 1);
    EXPECT_EQ(snaps[0].childrenIndices[0], 1);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST(ProcessModelTest, ConcurrentRefreshAndReadDoesNotCrash)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(1, "proc1", 'R', 100, 100, 1000, 1024, 0), makeCounter(2, "proc2", 'S', 200, 200, 1000, 2048, 1)});
    probe->setTotalCpuTime(100000);

    Domain::ProcessModel model(std::move(probe));

    std::atomic<bool> keepRunning{true};

    // Writer thread rapidly refreshes the model
    std::thread writer(
        [&]()
        {
            while (keepRunning)
            {
                model.refresh();
            }
        });

    // Reader thread rapidly copies snapshots
    std::thread reader(
        [&]()
        {
            while (keepRunning)
            {
                auto snaps = model.snapshots();
                if (!snaps.empty())
                {
                    EXPECT_EQ(snaps[0].pid, 1);
                }
            }
        });

    // Let them fight for 100ms
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    keepRunning = false;

    writer.join();
    reader.join();
}

// =============================================================================
// Watched-process samples (#1098, #1172)
// =============================================================================

namespace
{

double steadySeconds(Domain::ProcessModel::Clock::time_point time)
{
    return std::chrono::duration<double>(time.time_since_epoch()).count();
}

} // namespace

TEST(ProcessModelTest, WatchedSamplesCarryEachGenerationsOwnSampleTime)
{
    // #1098: Process Details stamped history with the UI frame time that noticed
    // a new generation. Each sample now carries the time the model sampled it
    // (its NowFunction), the timebase of the Overview's process aggregates.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    probe->setTotalCpuTime(100000);
    ManualClock clock;
    clock.advance(std::chrono::seconds(1000));
    const auto now = clock.now();
    Domain::ProcessModel model(std::move(probe), now);
    model.watchProcess(100);

    model.refresh();
    const double firstTime = steadySeconds(now());
    clock.advance(std::chrono::milliseconds(250));
    model.refresh();
    const double secondTime = steadySeconds(now());

    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 2U);
    EXPECT_DOUBLE_EQ(samples[0].sampleTimeSeconds, firstTime);
    EXPECT_DOUBLE_EQ(samples[1].sampleTimeSeconds, secondTime);
    EXPECT_EQ(samples[0].version, 1U);
    EXPECT_EQ(samples[1].version, 2U);
    ASSERT_NE(samples[1].snapshot, nullptr);
    EXPECT_EQ(samples[1].snapshot->pid, 100);

    // The same timebase as the model's own history timestamps.
    const auto timestamps = model.historyTimestamps();
    ASSERT_FALSE(timestamps.empty());
    EXPECT_DOUBLE_EQ(timestamps.back(), secondTime);
}

TEST(ProcessModelTest, WatchedSamplesKeepEveryGenerationPublishedBetweenPolls)
{
    // #1098: a reader that polls once per frame saw only the latest generation,
    // so two publishes between frames lost one. Every generation since the
    // reader's last poll is returned, oldest first.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.watchProcess(100);

    model.refresh();
    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 1U);
    const std::uint64_t lastSeen = samples.back().version;

    // Three generations before the next poll.
    for (std::uint64_t i = 1; i <= 3; ++i)
    {
        clock.advance(std::chrono::milliseconds(100));
        rawProbe->setCounters({makeCounter(100, "watched", 'R', 1000 + (100 * i), 0, 5000)});
        model.refresh();
    }

    samples.clear();
    ASSERT_TRUE(model.watchedSamplesSince(lastSeen, samples));
    ASSERT_EQ(samples.size(), 3U);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        EXPECT_EQ(samples[i].version, lastSeen + 1 + i) << "consecutive generations, none skipped";
        EXPECT_NE(samples[i].snapshot, nullptr);
    }
    EXPECT_LT(samples[0].sampleTimeSeconds, samples[1].sampleTimeSeconds);
    EXPECT_LT(samples[1].sampleTimeSeconds, samples[2].sampleTimeSeconds);
}

TEST(ProcessModelTest, WatchedSamplesSinceCopiesNothingWhenNothingNewWasPublished)
{
    // #1172: the selected process was looked up and deep-copied twice every
    // frame, new data or not. With nothing new published the poll returns false
    // and leaves the output untouched, and a sample handed out twice is the same
    // shared snapshot, not a fresh copy.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    probe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.watchProcess(100);
    model.refresh();

    std::vector<Domain::ProcessSample> first;
    ASSERT_TRUE(model.watchedSamplesSince(0, first));
    ASSERT_EQ(first.size(), 1U);

    std::vector<Domain::ProcessSample> unchanged;
    EXPECT_FALSE(model.watchedSamplesSince(first.back().version, unchanged));
    EXPECT_TRUE(unchanged.empty());

    std::vector<Domain::ProcessSample> again;
    ASSERT_TRUE(model.watchedSamplesSince(0, again));
    ASSERT_EQ(again.size(), 1U);
    EXPECT_EQ(again[0].snapshot.get(), first[0].snapshot.get()) << "shared, not deep-copied per poll";
}

TEST(ProcessModelTest, WatchProcessStartsFromTheCurrentGeneration)
{
    // Selecting a process shows it at once, from the generation already
    // published, rather than waiting for the next refresh.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "first", 'R', 1000, 0, 5000), makeCounter(200, "second", 'R', 1000, 0, 6000)});
    probe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    model.watchProcess(200);
    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 1U);
    EXPECT_EQ(samples[0].version, model.snapshotVersion());
    ASSERT_NE(samples[0].snapshot, nullptr);
    EXPECT_EQ(samples[0].snapshot->pid, 200);

    // Switching the watch drops the old process's samples.
    model.watchProcess(100);
    samples.clear();
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 1U);
    ASSERT_NE(samples[0].snapshot, nullptr);
    EXPECT_EQ(samples[0].snapshot->pid, 100);
}

TEST(ProcessModelTest, WatchedSampleOfAGenerationWithoutTheProcessIsEmpty)
{
    // A generation that no longer lists the process still gets a sample, with no
    // snapshot, so the reader can tell "exited" from "missed generations".
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    rawProbe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.watchProcess(100);
    model.refresh();
    rawProbe->setCounters({makeCounter(200, "other", 'R', 1000, 0, 6000)});
    model.refresh();

    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    ASSERT_EQ(samples.size(), 2U);
    EXPECT_NE(samples[0].snapshot, nullptr);
    EXPECT_EQ(samples[1].snapshot, nullptr);
    EXPECT_EQ(samples[1].version, 2U);
}

TEST(ProcessModelTest, WatchedSamplesBeyondTheRingLeaveAVersionGap)
{
    // More generations than the ring holds between two polls: the oldest are
    // gone, and the first sample returned is not the one after the reader's last,
    // so it can mark the gap.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    probe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.watchProcess(100);
    model.refresh();
    std::vector<Domain::ProcessSample> samples;
    ASSERT_TRUE(model.watchedSamplesSince(0, samples));
    const std::uint64_t lastSeen = samples.back().version;

    constexpr std::size_t EXTRA = 3;
    for (std::size_t i = 0; i < Domain::ProcessModel::WATCHED_SAMPLE_CAPACITY + EXTRA; ++i)
    {
        model.refresh();
    }

    samples.clear();
    ASSERT_TRUE(model.watchedSamplesSince(lastSeen, samples));
    ASSERT_EQ(samples.size(), Domain::ProcessModel::WATCHED_SAMPLE_CAPACITY);
    EXPECT_EQ(samples.front().version, lastSeen + 1 + EXTRA);
    EXPECT_EQ(samples.back().version, model.snapshotVersion());
}

TEST(ProcessModelTest, NoSamplesAreKeptWhileNothingIsWatched)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({makeCounter(100, "watched", 'R', 1000, 0, 5000)});
    probe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.watchProcess(100);
    model.refresh();
    model.watchProcess(-1);
    model.refresh();

    std::vector<Domain::ProcessSample> samples;
    EXPECT_FALSE(model.watchedSamplesSince(0, samples));
    EXPECT_TRUE(samples.empty());
}

// =============================================================================
// A throwing per-process GPU merge (#1142)
// =============================================================================

TEST(ProcessModelTest, ThrowingPerProcessGpuQueryStillPublishesProcesses)
{
    // #1142: readProcessGPUCounters() throwing escaped refresh() after the
    // per-process state had advanced, so a GPU probe that threw every time froze
    // the process list. The processes are now published without GPU fields.
    auto processProbe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = processProbe.get();
    rawProbe->setCounters({makeCounter(100, "gpu_process", 'R', 1000, 500)});
    rawProbe->setTotalCpuTime(100000);

    auto gpuProbe = std::make_unique<MockGPUProbe>();
    Platform::GPUCapabilities caps;
    caps.hasPerProcessMetrics = true;
    gpuProbe->withCapabilities(caps);
    gpuProbe->withGPU("GPU0", "Test GPU", "TestVendor").withProcessGPU(100, "GPU0", 512ULL * 1024 * 1024).withProcessCountersThrowing();
    auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
    gpuModel->refresh();

    Domain::ProcessModel processModel(std::move(processProbe));
    processModel.setGPUModel(gpuModel);

    ASSERT_NO_THROW(processModel.refresh());
    EXPECT_EQ(processModel.snapshotVersion(), 1U);

    rawProbe->setCounters({makeCounter(100, "gpu_process", 'R', 2000, 500), makeCounter(200, "new_process", 'R', 10, 5, 7000)});
    ASSERT_NO_THROW(processModel.refresh());
    EXPECT_EQ(processModel.snapshotVersion(), 2U) << "publication keeps going while the GPU query keeps throwing";

    const auto snaps = processModel.snapshots();
    ASSERT_EQ(snaps.size(), 2U);
    for (const auto& snap : snaps)
    {
        EXPECT_EQ(snap.gpuMemoryBytes, 0U);
        EXPECT_TRUE(snap.gpuDevices.empty());
    }
}

// =============================================================================
// Per-process values the probe could not read (#1110)
// =============================================================================

TEST(ProcessModelTest, UnreadableHandleCountIsUnavailableAndLeftOutOfTheTotal)
{
    // #1110: an unreadable FD/handle count was a 0 that the table showed and the
    // system total counted. It is now flagged unavailable and left out of the
    // total.
    auto probe = std::make_unique<MockProcessProbe>();
    auto readable = makeCounter(100, "mine", 'R', 1000, 0, 5000);
    readable.handleCount = 10;
    auto unreadable = makeCounter(200, "root_owned", 'S', 1000, 0, 6000);
    unreadable.handleCount = 50; // a placeholder that must not be shown or counted
    unreadable.handleCountAvailable = false;
    probe->setCounters({readable, unreadable});
    probe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();
    clock.advance(std::chrono::seconds(1));
    model.refresh(); // the aggregate history starts with the second refresh

    const auto snaps = model.snapshots();
    const auto mine = std::ranges::find(snaps, 100, &Domain::ProcessSnapshot::pid);
    const auto theirs = std::ranges::find(snaps, 200, &Domain::ProcessSnapshot::pid);
    ASSERT_NE(mine, snaps.end());
    ASSERT_NE(theirs, snaps.end());
    EXPECT_TRUE(mine->handleCountAvailable);
    EXPECT_EQ(mine->handleCount, 10);
    EXPECT_FALSE(theirs->handleCountAvailable);
    EXPECT_EQ(theirs->handleCount, 0);

    const auto handleTotals = model.systemHandleCountHistory();
    ASSERT_FALSE(handleTotals.empty());
    EXPECT_DOUBLE_EQ(handleTotals.back(), 10.0);
}

TEST(ProcessModelTest, IoRateNeedsBothReadingsToBeAvailable)
{
    // #1110: an I/O rate is taken between two readings. With the earlier one
    // unreadable (a placeholder 0), the next real reading would have counted the
    // process's lifetime of I/O as one interval's; that interval is unavailable
    // instead, and the one after is a real rate.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    auto counter = makeCounter(100, "proc", 'R', 1000, 0, 5000);
    counter.ioCountersAvailable = false;
    rawProbe->setCounters({counter});
    rawProbe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();
    EXPECT_FALSE(model.snapshots().at(0).ioAvailable);

    clock.advance(std::chrono::seconds(1));
    counter.ioCountersAvailable = true;
    counter.readBytes = 1'000'000'000;
    rawProbe->setCounters({counter});
    model.refresh();
    EXPECT_FALSE(model.snapshots().at(0).ioAvailable) << "no earlier reading to take a rate from";
    EXPECT_DOUBLE_EQ(model.snapshots().at(0).ioReadBytesPerSec, 0.0);

    clock.advance(std::chrono::seconds(1));
    counter.readBytes += 4096;
    rawProbe->setCounters({counter});
    model.refresh();
    EXPECT_TRUE(model.snapshots().at(0).ioAvailable);
    EXPECT_DOUBLE_EQ(model.snapshots().at(0).ioReadBytesPerSec, 4096.0);
}

TEST(ProcessModelTest, NetworkCountersTurnedOffDuringTheSampleAreUnavailableInThatSample)
{
    // #1302 review: the Windows probe can find its per-process network counters unusable during the
    // socket-traffic read that follows enumerate() (#1161). The counters enumerate() had returned were
    // already marked available, so that sample published a rate as a reading instead of unavailable.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    Platform::ProcessCapabilities withNetwork;
    withNetwork.hasNetworkCounters = true;
    rawProbe->setCapabilities(withNetwork);
    auto proc = makeCounter(100, "proc", 'R', 1000, 0, 5000);
    rawProbe->setCounters({proc});
    rawProbe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();
    ASSERT_TRUE(model.snapshots().at(0).networkAvailable);

    clock.advance(std::chrono::seconds(1));
    proc.netSentBytes = 2000;
    rawProbe->setCounters({proc});
    Platform::ProcessCapabilities revoked = withNetwork;
    revoked.hasNetworkCounters = false;
    rawProbe->switchCapabilitiesOnNextSocketRead(revoked);
    model.refresh();
    EXPECT_FALSE(model.snapshots().at(0).networkAvailable);
    EXPECT_DOUBLE_EQ(model.snapshots().at(0).netSentBytesPerSec, 0.0);
}

TEST(ProcessModelTest, UnattributableNetworkCountersAreUnavailableAndLeftOutOfTheTotal)
{
    // #1110: a process whose connections can't be attributed to it (another
    // user's, without root, on Linux) showed 0 B/s. Its network rates are now
    // unavailable and left out of the totals.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    auto mine = makeCounter(100, "mine", 'R', 1000, 0, 5000);
    auto theirs = makeCounter(200, "theirs", 'R', 1000, 0, 6000);
    theirs.networkCountersAvailable = false;
    rawProbe->setCounters({mine, theirs});
    rawProbe->setTotalCpuTime(100000);
    ManualClock clock;
    Domain::ProcessModel model(std::move(probe), clock.now());
    model.refresh();

    clock.advance(std::chrono::seconds(1));
    mine.netSentBytes = 2000;
    theirs.netSentBytes = 1'000'000; // a placeholder that must not be counted
    rawProbe->setCounters({mine, theirs});
    model.refresh();

    const auto snaps = model.snapshots();
    const auto theirsSnap = std::ranges::find(snaps, 200, &Domain::ProcessSnapshot::pid);
    ASSERT_NE(theirsSnap, snaps.end());
    EXPECT_FALSE(theirsSnap->networkAvailable);
    EXPECT_DOUBLE_EQ(theirsSnap->netSentBytesPerSec, 0.0);
    const auto mineSnap = std::ranges::find(snaps, 100, &Domain::ProcessSnapshot::pid);
    ASSERT_NE(mineSnap, snaps.end());
    EXPECT_TRUE(mineSnap->networkAvailable);
    EXPECT_DOUBLE_EQ(mineSnap->netSentBytesPerSec, 2000.0);

    const auto sentTotals = model.systemNetSentHistory();
    ASSERT_FALSE(sentTotals.empty());
    EXPECT_DOUBLE_EQ(sentTotals.back(), 2000.0);
}
