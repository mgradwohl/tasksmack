/// @file test_WindowsProcessProbeMath.cpp
/// @brief Unit tests for WindowsProcessProbeMath.h's pure process-state, detail-cache and EStats logic
///
/// WindowsProcessProbeMath.h includes no Windows header, so these tests build and run on every
/// platform, including Linux CI's sanitizer and coverage jobs (#1133). Tests that need the real
/// probe or Windows types (the MIB_TCPROW conversions) stay in test_WindowsProcessProbe.cpp.

#include "Domain/SocketTrafficAccumulator.h"
#include "Platform/CpuAffinity.h"
#include "Platform/ProcessTypes.h"
#include "Platform/Windows/WindowsProcessProbeMath.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform
{
namespace
{

// ==========================================================================
// calculateDetailTTLsFromTotalRAMBytes: pure RAM-tier logic, no OS calls
// required. This machine's actual RAM only ever exercises one tier via the
// real GlobalMemoryStatusEx()-backed member function, so these fabricated
// byte counts are the only way to reach the other four tiers.
// ==========================================================================

constexpr std::uint64_t ONE_GIB = 1024ULL * 1024 * 1024;

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, BelowTwoGibUsesMostAggressiveCaching)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(ONE_GIB); // 1 GiB
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(4000));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(15000));
}

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, TwoToFourGibUsesConservativeTier)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(2 * ONE_GIB);
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(3000));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(10000));
}

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, FourToEightGibUsesBalancedTier)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(4 * ONE_GIB);
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(2000));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(8000));
}

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, EightToSixteenGibUsesModerateTier)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(8 * ONE_GIB);
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(1500));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(6000));
}

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, SixteenGibAndAboveUsesMostResponsiveTier)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(16 * ONE_GIB);
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(1000));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(4000));

    // Well above the top tier threshold should stay on the same (top) tier.
    const auto ttlsHuge = calculateDetailTTLsFromTotalRAMBytes(256 * ONE_GIB);
    EXPECT_EQ(ttlsHuge.light, ttls.light);
    EXPECT_EQ(ttlsHuge.heavy, ttls.heavy);
}

TEST(CalculateDetailTTLsFromTotalRAMBytesTest, ZeroBytesFallsIntoLowestTier)
{
    const auto ttls = calculateDetailTTLsFromTotalRAMBytes(0);
    EXPECT_EQ(ttls.light, std::chrono::milliseconds(4000));
    EXPECT_EQ(ttls.heavy, std::chrono::milliseconds(15000));
}

// ---------------------------------------------------------------------------
// classifyEStatsRow (#1100): the per-row decision shared by the IPv4 and IPv6 EStats walks
// ---------------------------------------------------------------------------

TEST(MarkWindowsReadAvailabilityTest, WithoutPerProcessNetworkCountersNetworkIsUnavailableNotZero)
{
    // #1285: non-elevated, no process's network bytes are read; they must not pass for a 0 B/s reading.
    ProcessCounters counters{};
    markWindowsReadAvailability(counters, false);
    EXPECT_FALSE(counters.networkCountersAvailable);
    EXPECT_TRUE(counters.handleCountAvailable); // From the bulk snapshot, for every process
    EXPECT_TRUE(counters.ioCountersAvailable);
}

TEST(MarkWindowsReadAvailabilityTest, WithPerProcessNetworkCountersEveryReadingIsAvailable)
{
    ProcessCounters counters{};
    counters.handleCountAvailable = false;
    counters.ioCountersAvailable = false;
    counters.networkCountersAvailable = false;
    markWindowsReadAvailability(counters, true);
    EXPECT_TRUE(counters.networkCountersAvailable);
    EXPECT_TRUE(counters.handleCountAvailable);
    EXPECT_TRUE(counters.ioCountersAvailable);
}

namespace
{
[[nodiscard]] ProcessThreadTally tallyOf(std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> threads)
{
    ProcessThreadTally tally;
    for (const auto& [state, waitReason] : threads)
    {
        tally.add(state, waitReason);
    }
    return tally;
}
constexpr std::uint32_t WAIT_REASON_USER_REQUEST = 6; // An ordinary wait (WaitForSingleObject and the like)
constexpr std::uint32_t THREAD_STATE_INITIALIZED = 0;
constexpr std::uint32_t THREAD_STATE_TERMINATED = 4;
// winerror.h's ERROR_INVALID_PARAMETER, which this file can't include; the header's ESTATS_* codes
// stand in for NO_ERROR, ERROR_ACCESS_DENIED and ERROR_NOT_FOUND.
constexpr std::uint32_t INVALID_PARAMETER_STATUS = 87;
} // namespace

TEST(DeriveProcessStateTest, AnyRunningOrReadyThreadIsRunning)
{
    // #1156: Linux 'R' is running or runnable.
    for (const std::uint32_t runnable :
         {THREAD_STATE_READY, THREAD_STATE_RUNNING, THREAD_STATE_STANDBY, THREAD_STATE_TRANSITION, THREAD_STATE_DEFERRED_READY})
    {
        EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_USER_REQUEST}, {runnable, 0}}), false), 'R') << runnable;
    }
    // A running thread beside suspended ones: the process is not stopped.
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_SUSPENDED}, {THREAD_STATE_RUNNING, 0}}), false), 'R');
}

TEST(DeriveProcessStateTest, EveryThreadWaitingIsSleeping)
{
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_USER_REQUEST},
                                          {THREAD_STATE_GATE_WAIT, 0},
                                          {THREAD_STATE_WAITING_FOR_PROCESS_IN_SWAP, 0}}),
                                 false),
              'S');
    // Some threads suspended, others in ordinary waits: still sleeping, not stopped.
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_SUSPENDED}, {THREAD_STATE_WAITING, WAIT_REASON_USER_REQUEST}}),
                                 false),
              'S');
}

TEST(DeriveProcessStateTest, EveryThreadSuspendedIsStopped)
{
    // A suspended or frozen process, or one stopped in a debugger: Linux 'T'.
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_SUSPENDED}, {THREAD_STATE_WAITING, WAIT_REASON_WR_SUSPENDED}}),
                                 false),
              'T');
    // Initialized/terminated threads neither run nor wait, so they don't stop it reading as stopped.
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_WAITING, WAIT_REASON_SUSPENDED}, {THREAD_STATE_TERMINATED, 0}}), false), 'T');
}

TEST(DeriveProcessStateTest, NoThreadToJudgeByIsUnknownNeverZombie)
{
    // Minimal processes (Secure System) list no threads; Windows has no zombie state to report.
    EXPECT_EQ(deriveProcessState(ProcessThreadTally{}, false), '?');
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_INITIALIZED, 0}, {THREAD_STATE_TERMINATED, 0}}), false), '?');
}

TEST(DeriveProcessStateTest, SystemIdleProcessIsIdle)
{
    // Its threads run whenever a CPU is idle; that is not load.
    EXPECT_EQ(deriveProcessState(tallyOf({{THREAD_STATE_RUNNING, 0}, {THREAD_STATE_RUNNING, 0}}), true), 'I');
}

TEST(PlanDetailRefreshTest, FirstSampleRefreshesEverything)
{
    const auto plan = planDetailRefresh(true, false, false, false);
    EXPECT_TRUE(plan.light);
    EXPECT_TRUE(plan.heavy);
    EXPECT_TRUE(plan.priority);
}

TEST(PlanDetailRefreshTest, NothingDueAndNoPriorityChangeNeedsNoHandle)
{
    EXPECT_FALSE(planDetailRefresh(false, false, false, false).any());
}

TEST(PlanDetailRefreshTest, BasePriorityChangeRereadsOnlyThePriorityClass)
{
    // #1156: a Set Priority shows on the next sample, not up to a heavy TTL later.
    const auto plan = planDetailRefresh(false, false, false, true);
    EXPECT_TRUE(plan.priority);
    EXPECT_FALSE(plan.light);
    EXPECT_FALSE(plan.heavy);
}

TEST(PlanDetailRefreshTest, PriorityIsAlsoReadWithTheHeavyDetails)
{
    const auto heavy = planDetailRefresh(false, false, true, false);
    EXPECT_TRUE(heavy.heavy);
    EXPECT_TRUE(heavy.priority);
    const auto light = planDetailRefresh(false, true, false, false);
    EXPECT_TRUE(light.light);
    EXPECT_FALSE(light.heavy);
    EXPECT_FALSE(light.priority);
}

TEST(ClassifyEStatsRowTest, SaneEstablishedReadsAreReported)
{
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, 0, 1'000, 2'000), EStatsRowOutcome::Accumulated);
    // A just-opened connection reads OK with zero bytes; it is still reported, so Domain tracks it
    // from now on.
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, 0, 0, 0), EStatsRowOutcome::Accumulated);
}

TEST(ClassifyEStatsRowTest, NonEstablishedRowsAreSkipped)
{
    constexpr std::uint32_t LISTEN = 2;
    constexpr std::uint32_t TIME_WAIT = 11;

    EXPECT_EQ(classifyEStatsRow(LISTEN, 0, 100, 100), EStatsRowOutcome::SkippedState);
    EXPECT_EQ(classifyEStatsRow(TIME_WAIT, 0, 100, 100), EStatsRowOutcome::SkippedState);
}

TEST(ClassifyEStatsRowTest, FailedReadsAreNotReported)
{
    constexpr std::uint32_t ERROR_NOT_FOUND_CODE = 1168;
    constexpr std::uint32_t ERROR_ACCESS_DENIED_CODE = 5;

    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, ERROR_NOT_FOUND_CODE, 100, 100), EStatsRowOutcome::ReadFailed);
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, ERROR_ACCESS_DENIED_CODE, 100, 100), EStatsRowOutcome::ReadFailed);
}

TEST(ClassifyEStatsRowTest, CountersAboveOneTerabyteAreRejected)
{
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, 0, MAX_SANE_ESTATS_CONNECTION_BYTES + 1, 0), EStatsRowOutcome::Garbage);
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, 0, 0, MAX_SANE_ESTATS_CONNECTION_BYTES + 1), EStatsRowOutcome::Garbage);
    // Exactly 1 TB is still accepted (the cap is exclusive).
    EXPECT_EQ(classifyEStatsRow(TCP_STATE_ESTABLISHED, 0, MAX_SANE_ESTATS_CONNECTION_BYTES, 0), EStatsRowOutcome::Accumulated);
}

TEST(EStatsSampleCountsTest, Ipv4AndIpv6TalliesAdd)
{
    EStatsSampleCounts v4{
        .total = 10,
        .established = 4,
        .enabled = 4,
        .alreadyEnabled = 0,
        .readOk = 3,
        .saneReads = 2,
        .readNotFound = 1,
        .readFailedOther = 0,
        .accessDenied = 0,
        .hasData = 2,
        .garbage = 1,
    };
    const EStatsSampleCounts v6{
        .total = 5,
        .established = 2,
        .enabled = 1,
        .alreadyEnabled = 1,
        .readOk = 2,
        .saneReads = 2,
        .readNotFound = 0,
        .readFailedOther = 1,
        .accessDenied = 1,
        .hasData = 1,
        .garbage = 0,
    };
    v4 += v6;
    EXPECT_EQ(v4.total, 15U);
    EXPECT_EQ(v4.established, 6U);
    EXPECT_EQ(v4.enabled, 5U);
    EXPECT_EQ(v4.alreadyEnabled, 1U);
    EXPECT_EQ(v4.readOk, 5U);
    EXPECT_EQ(v4.saneReads, 4U);
    EXPECT_EQ(v4.readNotFound, 1U);
    EXPECT_EQ(v4.readFailedOther, 1U);
    EXPECT_EQ(v4.accessDenied, 1U);
    EXPECT_EQ(v4.hasData, 3U);
    EXPECT_EQ(v4.garbage, 1U);
}

TEST(RecordEStatsRowTest, TalliesEachOutcome)
{
    // The real per-row tally both table walks use (#1161): NOT_FOUND is counted apart from other
    // read failures, and a garbage read is a successful read but not a sane one.
    EStatsSampleCounts counts;
    constexpr std::uint32_t LISTEN = 2;
    constexpr std::uint64_t TOO_BIG = MAX_SANE_ESTATS_CONNECTION_BYTES + 1;

    (void) recordEStatsRow(counts, LISTEN, std::nullopt, ESTATS_NO_ERROR, 9, 9);                    // not counted
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, ESTATS_NO_ERROR, ESTATS_NO_ERROR, 10, 0); // sane, has data
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, ESTATS_NO_ERROR, ESTATS_NO_ERROR, 0, 0);  // sane, no data
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, ESTATS_NO_ERROR, ESTATS_NO_ERROR, TOO_BIG, 0);
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, ESTATS_ERROR_NOT_FOUND, ESTATS_ERROR_NOT_FOUND, 0, 0);
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, std::nullopt, INVALID_PARAMETER_STATUS, 0, 0);
    (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, ESTATS_ERROR_ACCESS_DENIED, ESTATS_ERROR_ACCESS_DENIED, 0, 0);

    EXPECT_EQ(counts.established, 6U);
    EXPECT_EQ(counts.enabled, 3U);
    EXPECT_EQ(counts.readOk, 3U);
    EXPECT_EQ(counts.saneReads, 2U);
    EXPECT_EQ(counts.hasData, 1U);
    EXPECT_EQ(counts.garbage, 1U);
    EXPECT_EQ(counts.readNotFound, 1U);
    EXPECT_EQ(counts.readFailedOther, 1U); // ACCESS_DENIED is tallied as accessDenied, not here
    EXPECT_EQ(counts.accessDenied, 1U);
}

// ---------------------------------------------------------------------------
// classifyEStatsProbe (#1161): does a real sample prove EStats works?
// ---------------------------------------------------------------------------

/// Replays a per-row (enableStatus, readStatus) error sequence for ESTABLISHED rows into the
/// tallies the probe's table walks produce, so each test reads as "the OS returned X, Y, Z".
struct EStatsRowResult
{
    std::uint32_t enableStatus = ESTATS_NO_ERROR;
    std::uint32_t readStatus = ESTATS_NO_ERROR;
    std::uint64_t bytesOut = 0;
    std::uint64_t bytesIn = 0;
};

EStatsSampleCounts tallyEstablishedRows(const std::vector<EStatsRowResult>& rows)
{
    EStatsSampleCounts counts;
    counts.total = rows.size();
    for (const auto& row : rows)
    {
        (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, row.enableStatus, row.readStatus, row.bytesOut, row.bytesIn);
    }
    return counts;
}

TEST(ClassifyEStatsProbeTest, AllAccessDeniedIsUnavailable)
{
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ESTATS_ERROR_ACCESS_DENIED, .readStatus = ESTATS_ERROR_ACCESS_DENIED},
        {.enableStatus = ESTATS_ERROR_ACCESS_DENIED, .readStatus = ESTATS_ERROR_ACCESS_DENIED},
    });
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Unavailable);
}

TEST(ClassifyEStatsProbeTest, AnyAccessDeniedIsUnavailableEvenIfSomeReadsWork)
{
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ESTATS_NO_ERROR, .readStatus = ESTATS_NO_ERROR},
        {.enableStatus = ESTATS_ERROR_ACCESS_DENIED, .readStatus = ESTATS_ERROR_NOT_FOUND},
    });
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Unavailable);
}

TEST(ClassifyEStatsProbeTest, DummyRowNotFoundThenEveryRealReadFailingIsUnavailable)
{
    // The #1161 case: the constructor's dummy-row probe returned ERROR_NOT_FOUND (which the old
    // detection treated as "available"), then every real established connection's Set/Get
    // fails without ever saying ACCESS_DENIED. The old code kept hasNetworkCounters = true and
    // showed 0 B for every process with no lock icon; a real sample now proves it unavailable.
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ESTATS_ERROR_NOT_FOUND, .readStatus = ESTATS_ERROR_NOT_FOUND},
        {.enableStatus = INVALID_PARAMETER_STATUS, .readStatus = ESTATS_ERROR_NOT_FOUND},
        {.enableStatus = ESTATS_ERROR_NOT_FOUND, .readStatus = INVALID_PARAMETER_STATUS},
    });
    ASSERT_EQ(counts.accessDenied, 0U);
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Unavailable);
}

TEST(ClassifyEStatsProbeTest, ZeroEstablishedConnectionsIsUndetermined)
{
    // Nothing to read proves nothing either way: keep trying on the next sample rather than
    // declaring the feature dead on an idle machine.
    EXPECT_EQ(classifyEStatsProbe(EStatsSampleCounts{}), EStatsProbeResult::Undetermined);

    EStatsSampleCounts onlyListeners;
    onlyListeners.total = 40; // e.g. all LISTEN / TIME_WAIT rows
    EXPECT_EQ(classifyEStatsProbe(onlyListeners), EStatsProbeResult::Undetermined);
}

TEST(ClassifyEStatsProbeTest, SuccessfulReadsAreAvailable)
{
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ESTATS_NO_ERROR, .readStatus = ESTATS_NO_ERROR},
        {.enableStatus = ESTATS_NO_ERROR, .readStatus = ESTATS_NO_ERROR},
    });
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Available);
}

TEST(ClassifyEStatsProbeTest, ReadsWorkingWithoutEnableAreAvailable)
{
    // Collection may already have been enabled by another (elevated) process, so a failed
    // enable with a successful read still proves the counters are real.
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ESTATS_ERROR_NOT_FOUND, .readStatus = ESTATS_NO_ERROR},
        {.enableStatus = ESTATS_ERROR_NOT_FOUND, .readStatus = ESTATS_ERROR_NOT_FOUND}, // connection closed mid-walk
    });
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Available);
}

TEST(ClassifyEStatsProbeTest, OnlyNotFoundReadsAreUndetermined)
{
    // Every snapshotted connection closed before its EStats read: ERROR_NOT_FOUND for all of
    // them is an ordinary race, not proof the API is unusable. Before this fix one such sample
    // permanently disabled the network column.
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ESTATS_ERROR_NOT_FOUND, .readStatus = ESTATS_ERROR_NOT_FOUND},
        {.enableStatus = ESTATS_ERROR_NOT_FOUND, .readStatus = ESTATS_ERROR_NOT_FOUND},
    });
    ASSERT_EQ(counts.readNotFound, 2U);
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Undetermined);
}

TEST(ClassifyEStatsProbeTest, NotFoundSampleThenSuccessfulSampleIsAvailable)
{
    const auto raced = tallyEstablishedRows({{.enableStatus = ESTATS_ERROR_NOT_FOUND, .readStatus = ESTATS_ERROR_NOT_FOUND}});
    ASSERT_EQ(classifyEStatsProbe(raced, 0), EStatsProbeResult::Undetermined);

    // The probe counted one inconclusive sample; the next one reads a live connection.
    const auto next =
        tallyEstablishedRows({{.enableStatus = ESTATS_NO_ERROR, .readStatus = ESTATS_NO_ERROR, .bytesOut = 512, .bytesIn = 2048}});
    EXPECT_EQ(classifyEStatsProbe(next, 1), EStatsProbeResult::Available);
}

TEST(ClassifyEStatsProbeTest, ReadAccessDeniedIsUnavailable)
{
    // ACCESS_DENIED from the read alone (enable succeeded or was skipped) is just as conclusive.
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ESTATS_NO_ERROR, .readStatus = ESTATS_ERROR_ACCESS_DENIED},
        {.enableStatus = ESTATS_ERROR_NOT_FOUND, .readStatus = ESTATS_ERROR_NOT_FOUND},
    });
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Unavailable);
}

TEST(ClassifyEStatsProbeTest, GarbageOnlyReadsAreNotAvailable)
{
    // A > 1 TB counter is rejected and never reported, so a sample of only garbage reads
    // proves nothing; it used to count as a successful read and verify EStats with no data.
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ESTATS_NO_ERROR, .readStatus = ESTATS_NO_ERROR, .bytesOut = MAX_SANE_ESTATS_CONNECTION_BYTES + 1},
        {.enableStatus = ESTATS_NO_ERROR, .readStatus = ESTATS_NO_ERROR, .bytesIn = MAX_SANE_ESTATS_CONNECTION_BYTES + 1},
    });
    ASSERT_EQ(counts.readOk, 2U);
    ASSERT_EQ(counts.saneReads, 0U);
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Undetermined);
}

TEST(ClassifyEStatsProbeTest, InconclusiveSamplesInARowBecomeUnavailable)
{
    // A race does not repeat on every sample: after MAX_INCONCLUSIVE_ESTATS_SAMPLES consecutive
    // NOT_FOUND/garbage-only samples the reads plainly never work, so stop claiming the column.
    const auto notFound = tallyEstablishedRows({{.enableStatus = ESTATS_ERROR_NOT_FOUND, .readStatus = ESTATS_ERROR_NOT_FOUND}});
    const auto garbage = tallyEstablishedRows({{.readStatus = ESTATS_NO_ERROR, .bytesOut = MAX_SANE_ESTATS_CONNECTION_BYTES + 1}});
    for (std::size_t prior = 0; prior + 1 < MAX_INCONCLUSIVE_ESTATS_SAMPLES; ++prior)
    {
        EXPECT_EQ(classifyEStatsProbe(notFound, prior), EStatsProbeResult::Undetermined) << prior;
        EXPECT_EQ(classifyEStatsProbe(garbage, prior), EStatsProbeResult::Undetermined) << prior;
    }
    EXPECT_EQ(classifyEStatsProbe(notFound, MAX_INCONCLUSIVE_ESTATS_SAMPLES - 1), EStatsProbeResult::Unavailable);
    EXPECT_EQ(classifyEStatsProbe(garbage, MAX_INCONCLUSIVE_ESTATS_SAMPLES - 1), EStatsProbeResult::Unavailable);

    // An idle sample is never inconclusive in that sense, whatever the streak.
    EXPECT_EQ(classifyEStatsProbe(EStatsSampleCounts{}, MAX_INCONCLUSIVE_ESTATS_SAMPLES), EStatsProbeResult::Undetermined);
}

// ---------------------------------------------------------------------------
// EStatsEnableTracker (#1418): enable EStats collection once per connection, not every sample
// ---------------------------------------------------------------------------

constexpr std::uint64_t KEY_A = 0x1111;
constexpr std::uint64_t KEY_B = 0x2222;
constexpr std::uint64_t KEY_C = 0x3333;

/// Record a sane read with data for @p key, as the probe would after a successful enable (or with
/// none attempted when needsEnable() said it was already on).
void recordSaneRead(EStatsEnableTracker& tracker, std::uint64_t key)
{
    const std::optional<std::uint32_t> enableStatus =
        tracker.needsEnable(key) ? std::optional<std::uint32_t>{ESTATS_NO_ERROR} : std::nullopt;
    tracker.record(key, enableStatus, EStatsRowOutcome::Accumulated, true);
}

TEST(EStatsEnableTrackerTest, NewConnectionNeedsEnable)
{
    const EStatsEnableTracker tracker;
    EXPECT_TRUE(tracker.needsEnable(KEY_A));
    EXPECT_EQ(tracker.size(), 0U);
}

TEST(EStatsEnableTrackerTest, EnabledConnectionIsNotEnabledAgain)
{
    EStatsEnableTracker tracker;
    tracker.record(KEY_A, ESTATS_NO_ERROR, EStatsRowOutcome::Accumulated, true);
    tracker.endSample(true);
    EXPECT_FALSE(tracker.needsEnable(KEY_A));
    EXPECT_TRUE(tracker.needsEnable(KEY_B));

    // Next sample: no enable attempted (nullopt) and the read works, so it stays known.
    tracker.record(KEY_A, std::nullopt, EStatsRowOutcome::Accumulated, true);
    tracker.endSample(true);
    EXPECT_FALSE(tracker.needsEnable(KEY_A));
}

TEST(EStatsEnableTrackerTest, FailedEnableIsNotRemembered)
{
    // Every failed enable must reach recordEStatsRow() again next sample, so the #1161 / #1358
    // access-denied detection sees it.
    for (const std::uint32_t status : {ESTATS_ERROR_ACCESS_DENIED, ESTATS_ERROR_NOT_FOUND, std::uint32_t{87}})
    {
        EStatsEnableTracker tracker;
        // Even if the read works (another process enabled collection), our enable did not.
        tracker.record(KEY_A, status, EStatsRowOutcome::Accumulated, true);
        tracker.endSample(true);
        EXPECT_TRUE(tracker.needsEnable(KEY_A)) << status;
    }
}

TEST(EStatsEnableTrackerTest, ReadWithoutAnyEnableIsNotRemembered)
{
    // No Set function: nothing was enabled, whatever the read says.
    EStatsEnableTracker tracker;
    tracker.record(KEY_A, std::nullopt, EStatsRowOutcome::Accumulated, true);
    tracker.endSample(true);
    EXPECT_TRUE(tracker.needsEnable(KEY_A));
}

TEST(EStatsEnableTrackerTest, FailedReadEnablesAgainNextSample)
{
    for (const EStatsRowOutcome outcome : {EStatsRowOutcome::ReadFailed, EStatsRowOutcome::Garbage})
    {
        EStatsEnableTracker tracker;
        recordSaneRead(tracker, KEY_A);
        tracker.endSample(true);
        ASSERT_FALSE(tracker.needsEnable(KEY_A));

        tracker.record(KEY_A, std::nullopt, outcome, false);
        tracker.endSample(true);
        EXPECT_TRUE(tracker.needsEnable(KEY_A)) << static_cast<int>(outcome);
    }
}

TEST(EStatsEnableTrackerTest, ReadWithNoDataEnablesAgainNextSample)
{
    // Both counters 0: collection may be off (a reused 4-tuple, or another tool disabled it).
    EStatsEnableTracker tracker;
    tracker.record(KEY_A, ESTATS_NO_ERROR, EStatsRowOutcome::Accumulated, false);
    tracker.endSample(true);
    EXPECT_TRUE(tracker.needsEnable(KEY_A));

    recordSaneRead(tracker, KEY_A);
    tracker.endSample(true);
    ASSERT_FALSE(tracker.needsEnable(KEY_A));
    tracker.record(KEY_A, std::nullopt, EStatsRowOutcome::Accumulated, false);
    tracker.endSample(true);
    EXPECT_TRUE(tracker.needsEnable(KEY_A));
}

TEST(EStatsEnableTrackerTest, ReusedFourTupleIsReEnabled)
{
    // A connection closes and a new one takes the same 4-tuple (same key) between samples. The new
    // one was never enabled, so its read fails or reads 0 bytes: forgotten, enabled next sample.
    EStatsEnableTracker tracker;
    recordSaneRead(tracker, KEY_A);
    tracker.endSample(true);
    ASSERT_FALSE(tracker.needsEnable(KEY_A));

    tracker.record(KEY_A, std::nullopt, EStatsRowOutcome::Accumulated, false);
    tracker.endSample(true);
    EXPECT_TRUE(tracker.needsEnable(KEY_A));
}

TEST(EStatsEnableTrackerTest, ClosedConnectionsArePruned)
{
    EStatsEnableTracker tracker;
    recordSaneRead(tracker, KEY_A);
    recordSaneRead(tracker, KEY_B);
    tracker.endSample(true);
    ASSERT_EQ(tracker.size(), 2U);

    // KEY_B closed: not in this complete walk.
    recordSaneRead(tracker, KEY_A);
    tracker.endSample(true);
    EXPECT_EQ(tracker.size(), 1U);
    EXPECT_FALSE(tracker.needsEnable(KEY_A));
    EXPECT_TRUE(tracker.needsEnable(KEY_B));
}

TEST(EStatsEnableTrackerTest, IncompleteWalkPrunesNothing)
{
    // A table that could not be read leaves its connections out of the walk; they may still be open.
    EStatsEnableTracker tracker;
    recordSaneRead(tracker, KEY_A);
    recordSaneRead(tracker, KEY_B);
    tracker.endSample(true);

    recordSaneRead(tracker, KEY_A);
    tracker.endSample(false);
    EXPECT_FALSE(tracker.needsEnable(KEY_B));

    // The next complete walk prunes whatever it does not see.
    recordSaneRead(tracker, KEY_A);
    tracker.endSample(true);
    EXPECT_TRUE(tracker.needsEnable(KEY_B));
}

TEST(EStatsEnableTrackerTest, SkippedRowsAreIgnored)
{
    EStatsEnableTracker tracker;
    recordSaneRead(tracker, KEY_A);
    tracker.record(KEY_A, std::nullopt, EStatsRowOutcome::SkippedState, false);
    tracker.endSample(true);
    EXPECT_FALSE(tracker.needsEnable(KEY_A));
}

TEST(EStatsEnableTrackerTest, SteadyConnectionsAreEnabledOnce)
{
    // The #1418 regression: three busy connections over five samples take three enables, not 15.
    EStatsEnableTracker tracker;
    std::size_t enables = 0;
    for (int sample = 0; sample < 5; ++sample)
    {
        for (const std::uint64_t key : {KEY_A, KEY_B, KEY_C})
        {
            enables += tracker.needsEnable(key) ? 1U : 0U;
            recordSaneRead(tracker, key);
        }
        tracker.endSample(true);
    }
    EXPECT_EQ(enables, 3U);
}

// classifyEStatsProbe() on samples where every connection was enabled earlier (#1418): the
// rows are tallied with no enable status, and the verdicts must match re-enabling them first.

EStatsSampleCounts tallyKnownRows(std::initializer_list<std::uint32_t> readStatuses, std::uint64_t bytes)
{
    EStatsSampleCounts counts;
    for (const std::uint32_t readStatus : readStatuses)
    {
        (void) recordEStatsRow(counts, TCP_STATE_ESTABLISHED, std::nullopt, readStatus, bytes, bytes);
    }
    return counts;
}

TEST(ClassifyEStatsProbeTest, AllKnownConnectionsReadingSanelyAreAvailable)
{
    const EStatsSampleCounts counts = tallyKnownRows({ESTATS_NO_ERROR, ESTATS_NO_ERROR}, 100);
    EXPECT_EQ(counts.enabled, 0U);
    EXPECT_EQ(counts.established, 2U);
    EXPECT_EQ(counts.saneReads, 2U);
    EXPECT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Available);
    EXPECT_EQ(classifyEStatsProbe(counts, MAX_INCONCLUSIVE_ESTATS_SAMPLES - 1), EStatsProbeResult::Available);

    // Idle known connections (0 bytes) still read sanely.
    EXPECT_EQ(classifyEStatsProbe(tallyKnownRows({ESTATS_NO_ERROR}, 0)), EStatsProbeResult::Available);
}

TEST(ClassifyEStatsProbeTest, AllKnownConnectionsMatchTheReEnabledVerdicts)
{
    // Same reads, with and without a (successful) enable first: same verdict every time.
    for (const std::uint32_t readStatus : {ESTATS_NO_ERROR, ESTATS_ERROR_ACCESS_DENIED, ESTATS_ERROR_NOT_FOUND, std::uint32_t{87}})
    {
        EStatsSampleCounts known;
        EStatsSampleCounts reEnabled;
        (void) recordEStatsRow(known, TCP_STATE_ESTABLISHED, std::nullopt, readStatus, 10, 10);
        (void) recordEStatsRow(reEnabled, TCP_STATE_ESTABLISHED, ESTATS_NO_ERROR, readStatus, 10, 10);
        for (std::size_t prior = 0; prior < MAX_INCONCLUSIVE_ESTATS_SAMPLES; ++prior)
        {
            EXPECT_EQ(classifyEStatsProbe(known, prior), classifyEStatsProbe(reEnabled, prior)) << readStatus << " " << prior;
        }
    }
    // A read denied on a known connection is still caught (#1161, #1358).
    EXPECT_EQ(classifyEStatsProbe(tallyKnownRows({ESTATS_ERROR_ACCESS_DENIED}, 0)), EStatsProbeResult::Unavailable);
}

// ---------------------------------------------------------------------------
// estatsConnectionKey (#1256): a stable per-connection key for SocketTrafficAccumulator
// ---------------------------------------------------------------------------

TcpConnectionEndpoints ipv4Endpoints(std::uint8_t localLast, std::uint32_t localPort, std::uint8_t remoteLast, std::uint32_t remotePort)
{
    TcpConnectionEndpoints endpoints;
    endpoints.family = TcpAddressFamily::IPv4;
    endpoints.localAddr = {192, 168, 1, localLast};
    endpoints.localPort = localPort;
    endpoints.remoteAddr = {10, 0, 0, remoteLast};
    endpoints.remotePort = remotePort;
    return endpoints;
}

TEST(EStatsConnectionKeyTest, SameConnectionHasTheSameKey)
{
    const auto endpoints = ipv4Endpoints(10, 0xBB01, 20, 0xD2C3);
    EXPECT_EQ(estatsConnectionKey(endpoints), estatsConnectionKey(endpoints));
    EXPECT_NE(estatsConnectionKey(endpoints), 0U);
    EXPECT_NE(estatsConnectionKey(TcpConnectionEndpoints{}), 0U); // 0 is reserved by the accumulator
}

TEST(EStatsConnectionKeyTest, UndefinedUpperPortBitsDoNotChangeTheKey)
{
    // The owner-PID tables leave the upper 16 bits of the port DWORDs undefined.
    const auto clean = ipv4Endpoints(10, 0x0000BB01U, 20, 0x0000D2C3U);
    const auto dirty = ipv4Endpoints(10, 0xDEADBB01U, 20, 0x1234D2C3U);
    EXPECT_EQ(estatsConnectionKey(clean), estatsConnectionKey(dirty));
}

TEST(EStatsConnectionKeyTest, SwappedEndpointsAreDifferentConnections)
{
    // Both ends of a loopback connection are in the table, owned by different processes.
    auto forward = ipv4Endpoints(1, 0x1111, 1, 0x2222);
    forward.localAddr = {127, 0, 0, 1};
    forward.remoteAddr = {127, 0, 0, 1};
    auto backward = forward;
    std::swap(backward.localPort, backward.remotePort);
    EXPECT_NE(estatsConnectionKey(forward), estatsConnectionKey(backward));

    const auto a = ipv4Endpoints(10, 0x1111, 20, 0x2222);
    TcpConnectionEndpoints b = a;
    std::swap(b.localAddr, b.remoteAddr);
    std::swap(b.localPort, b.remotePort);
    EXPECT_NE(estatsConnectionKey(a), estatsConnectionKey(b));
}

TEST(EStatsConnectionKeyTest, EachFieldDistinguishesConnections)
{
    const auto base = ipv4Endpoints(10, 0xBB01, 20, 0xD2C3);
    const std::uint64_t baseKey = estatsConnectionKey(base);
    EXPECT_NE(estatsConnectionKey(ipv4Endpoints(11, 0xBB01, 20, 0xD2C3)), baseKey);
    EXPECT_NE(estatsConnectionKey(ipv4Endpoints(10, 0xBB02, 20, 0xD2C3)), baseKey);
    EXPECT_NE(estatsConnectionKey(ipv4Endpoints(10, 0xBB01, 21, 0xD2C3)), baseKey);
    EXPECT_NE(estatsConnectionKey(ipv4Endpoints(10, 0xBB01, 20, 0xD2C4)), baseKey);

    TcpConnectionEndpoints v6;
    v6.family = TcpAddressFamily::IPv6;
    v6.localAddr = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    v6.remoteAddr = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02};
    v6.localPort = 0x1111;
    v6.remotePort = 0x2222;
    v6.localScopeId = 7;
    v6.remoteScopeId = 7;
    TcpConnectionEndpoints otherScope = v6;
    otherScope.remoteScopeId = 8; // the same link-local addresses on another interface
    EXPECT_NE(estatsConnectionKey(v6), estatsConnectionKey(otherScope));
}

TEST(EStatsConnectionKeyTest, Ipv4AndIpv6KeysNeverCollide)
{
    // The family is the key's top bit, so no IPv4 key can equal an IPv6 one -- not even for the
    // same address bytes and ports.
    constexpr std::uint64_t FAMILY_BIT = 1ULL << 63U;
    for (std::uint8_t last = 0; last < 64; ++last)
    {
        const auto v4 = ipv4Endpoints(last, 0x1111, last, 0x2222);
        TcpConnectionEndpoints v6 = v4;
        v6.family = TcpAddressFamily::IPv6;
        EXPECT_EQ(estatsConnectionKey(v4) & FAMILY_BIT, 0U);
        EXPECT_EQ(estatsConnectionKey(v6) & FAMILY_BIT, FAMILY_BIT);
    }
}

// ---------------------------------------------------------------------------
// EStats walks through Domain::SocketTrafficAccumulator (#1256): a process's network counter is
// monotonic, whatever its connections do between samples
// ---------------------------------------------------------------------------

/// Feeds fabricated EStats walks through makeSocketTrafficReading() and the accumulator
/// ProcessModel uses, as readSocketTraffic() and ProcessModel::refresh() do.
struct EStatsTrafficHarness
{
    static constexpr std::uint32_t PID = 4242;
    static constexpr std::uint64_t START_TICKS = 1'000;
    static constexpr std::uint64_t SECOND_NS = 1'000'000'000ULL;

    Domain::SocketTrafficAccumulator accumulator;
    std::uint64_t nowNs = 0;

    static EStatsConnectionRead good(std::uint64_t key, std::uint64_t received, std::uint64_t sent, std::uint32_t pid = PID)
    {
        return {.key = key, .pid = pid, .outcome = EStatsRowOutcome::Accumulated, .bytesReceived = received, .bytesSent = sent};
    }

    static EStatsConnectionRead failed(std::uint64_t key, EStatsRowOutcome outcome = EStatsRowOutcome::ReadFailed)
    {
        return {.key = key, .pid = PID, .outcome = outcome};
    }

    /// One sample: returns the process's (received, sent) totals.
    std::pair<std::uint64_t, std::uint64_t>
    sample(const std::vector<EStatsConnectionRead>& reads, bool complete = true, std::uint64_t startTicks = START_TICKS)
    {
        nowNs += SECOND_NS;
        std::vector<ProcessCounters> processes(1);
        processes[0].pid = static_cast<std::int32_t>(PID);
        processes[0].startTimeTicks = startTicks;
        accumulator.apply(makeSocketTrafficReading(reads, complete, nowNs), processes);
        return {processes[0].netReceivedBytes, processes[0].netSentBytes};
    }
};

TEST(EStatsSocketTrafficTest, ClosingAConnectionDoesNotLowerTheProcessTotal)
{
    // The old per-PID sum read 1'000 + 100 = 1'100, then 200 once B closed: a negative delta, and
    // the process showed 0 B/s however much A moved (#1256).
    EStatsTrafficHarness h;
    (void) h.sample({EStatsTrafficHarness::good(1, 100, 10), EStatsTrafficHarness::good(2, 1'000, 100)}); // baseline
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 150, 20), EStatsTrafficHarness::good(2, 1'500, 200)}),
              std::make_pair(std::uint64_t{550}, std::uint64_t{110}));
    // B closed; A moved 50 more in each direction.
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 200, 70)}), std::make_pair(std::uint64_t{600}, std::uint64_t{160}));
    // Every connection closed: the total holds.
    EXPECT_EQ(h.sample({}), std::make_pair(std::uint64_t{600}, std::uint64_t{160}));
}

TEST(EStatsSocketTrafficTest, Ipv4AndIpv6ConnectionsOfOneProcessAddUp)
{
    // Before #1100 only IPv4 was walked; both families now feed the same reading.
    TcpConnectionEndpoints v4;
    v4.localPort = 0x1111;
    TcpConnectionEndpoints v6 = v4;
    v6.family = TcpAddressFamily::IPv6;
    const std::uint64_t key4 = estatsConnectionKey(v4);
    const std::uint64_t key6 = estatsConnectionKey(v6);

    EStatsTrafficHarness h;
    (void) h.sample({EStatsTrafficHarness::good(key4, 0, 0), EStatsTrafficHarness::good(key6, 0, 0)});
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(key4, 1'000, 2'000), EStatsTrafficHarness::good(key6, 30'000, 40'000)}),
              std::make_pair(std::uint64_t{31'000}, std::uint64_t{42'000}));
}

TEST(EStatsSocketTrafficTest, AFailedRowReadDoesNotSpike)
{
    // A connection whose EStats read fails for one sample (or reads garbage) is reported unreadable
    // and stays open in Domain. Left out, it would look closed and then new: its 10'000 lifetime
    // bytes would land in one interval. Read again, it re-baselines rather than crediting the growth
    // from across the failed sample (#1346 review), then counts its growth as before.
    for (const EStatsRowOutcome outcome : {EStatsRowOutcome::ReadFailed, EStatsRowOutcome::Garbage})
    {
        EStatsTrafficHarness h;
        (void) h.sample({EStatsTrafficHarness::good(1, 10'000, 5'000)}); // baseline
        EXPECT_EQ(h.sample({EStatsTrafficHarness::failed(1, outcome)}), std::make_pair(std::uint64_t{0}, std::uint64_t{0}));
        EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 10'300, 5'030)}), std::make_pair(std::uint64_t{0}, std::uint64_t{0}));
        EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 10'500, 5'050)}), std::make_pair(std::uint64_t{200}, std::uint64_t{20}));
    }
}

TEST(EStatsSocketTrafficTest, AFailedReadIsReportedUnreadable)
{
    // The probe keeps no per-connection state (#1256): a failed or garbage read is reported as an
    // unreadable sample of a connection still in the table, whatever came before it; rows not in
    // ESTABLISHED are left out.
    const std::vector<EStatsConnectionRead> reads{
        EStatsTrafficHarness::good(1, 100, 10),
        EStatsTrafficHarness::failed(7),
        EStatsTrafficHarness::failed(8, EStatsRowOutcome::Garbage),
        {.key = 9, .pid = 1, .outcome = EStatsRowOutcome::SkippedState},
    };
    const auto samples = buildSocketTrafficSamples(reads);
    ASSERT_EQ(samples.size(), 3U);
    EXPECT_TRUE(samples[0].readable);
    EXPECT_EQ(samples[0].bytesReceived, 100U);
    EXPECT_EQ(samples[0].bytesSent, 10U);
    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        EXPECT_FALSE(samples[i].readable);
        EXPECT_EQ(samples[i].pid, static_cast<std::int32_t>(EStatsTrafficHarness::PID));
    }
    EXPECT_EQ(samples[1].key, 7U);
    EXPECT_EQ(samples[2].key, 8U);
}

TEST(EStatsSocketTrafficTest, AConnectionFirstReadFailedDoesNotCreditItsLifetimeBytes)
{
    // A connection already open when its first read fails: once it reads, its 50'000 lifetime bytes
    // only set the baseline instead of landing in one interval (#1256).
    EStatsTrafficHarness h;
    (void) h.sample({EStatsTrafficHarness::good(1, 0, 0)}); // baseline
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 10, 1), EStatsTrafficHarness::failed(2)}),
              std::make_pair(std::uint64_t{10}, std::uint64_t{1}));
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 20, 2), EStatsTrafficHarness::good(2, 50'000, 5'000)}),
              std::make_pair(std::uint64_t{20}, std::uint64_t{2}));
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 20, 2), EStatsTrafficHarness::good(2, 50'400, 5'040)}),
              std::make_pair(std::uint64_t{420}, std::uint64_t{42}));
}

TEST(EStatsSocketTrafficTest, ASampleWithAnUnreadableTableIsSkipped)
{
    // If the IPv4 or IPv6 table can't be read, its connections are missing from the walk. Reported,
    // they'd look closed and then new; the sample reports no reading instead, and the next complete
    // one measures from the last.
    EStatsTrafficHarness h;
    (void) h.sample({EStatsTrafficHarness::good(1, 1'000, 100), EStatsTrafficHarness::good(2, 2'000, 200)}); // baseline

    const auto partial = makeSocketTrafficReading({}, false, 123);
    EXPECT_EQ(partial.sampleTimeNs, 0U);
    EXPECT_TRUE(partial.sockets.empty());

    // Only connection 1's table was read this sample: skipped, totals held.
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 1'100, 110)}, false), std::make_pair(std::uint64_t{0}, std::uint64_t{0}));
    // Both back: only the growth since the baseline is credited.
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 1'300, 130), EStatsTrafficHarness::good(2, 2'500, 250)}),
              std::make_pair(std::uint64_t{800}, std::uint64_t{80}));
}

TEST(EStatsSocketTrafficTest, AReusedPidStartsFromZero)
{
    // A process identified by PID and start time: a new process reusing the PID doesn't inherit
    // the old one's bytes (SocketTrafficAccumulator).
    EStatsTrafficHarness h;
    (void) h.sample({EStatsTrafficHarness::good(1, 0, 0)});
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(1, 500, 50)}), std::make_pair(std::uint64_t{500}, std::uint64_t{50}));
    constexpr std::uint64_t NEW_START = EStatsTrafficHarness::START_TICKS + 1;
    EXPECT_EQ(h.sample({EStatsTrafficHarness::good(2, 40, 4)}, true, NEW_START), std::make_pair(std::uint64_t{40}, std::uint64_t{4}));
}

// ==========================================================================
// CPU affinity across processor groups (#1247). This machine has one group,
// so these fabricated layouts are the only coverage of the multi-group path.
// ==========================================================================

constexpr std::uint64_t FULL_GROUP = ~std::uint64_t{0};

CpuAffinity cpuList(std::string_view text)
{
    return CpuAffinity::fromCpuList(text).value_or(CpuAffinity{});
}

TEST(CpuAffinityFromGroupMasksTest, OneGroupMatchesTheSingleMask)
{
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 16, .activeMask = 0xFFFF}};
    const std::vector<GroupAffinityMask> masks{{.group = 0, .mask = 0x00F5}};
    EXPECT_EQ(cpuAffinityFromGroupMasks(masks, groups), CpuAffinity::fromMask(0x00F5));

    const std::vector<ProcessorGroupLayout> full{{.maximumProcessors = 64, .activeMask = FULL_GROUP}};
    const std::vector<GroupAffinityMask> all{{.group = 0, .mask = FULL_GROUP}};
    EXPECT_EQ(cpuAffinityFromGroupMasks(all, full), CpuAffinity::fromMask(FULL_GROUP));
}

TEST(CpuAffinityFromGroupMasksTest, TwoFullGroupsOf64AreCpus0To127)
{
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 64, .activeMask = FULL_GROUP},
                                                   {.maximumProcessors = 64, .activeMask = FULL_GROUP}};
    const std::vector<GroupAffinityMask> masks{{.group = 0, .mask = FULL_GROUP}, {.group = 1, .mask = FULL_GROUP}};
    const CpuAffinity affinity = cpuAffinityFromGroupMasks(masks, groups);
    EXPECT_EQ(affinity, cpuList("0-127"));
    EXPECT_EQ(affinity.count(), 128U);
}

TEST(CpuAffinityFromGroupMasksTest, GroupOrderInTheListDoesNotMatter)
{
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 64, .activeMask = FULL_GROUP},
                                                   {.maximumProcessors = 64, .activeMask = FULL_GROUP}};
    const std::vector<GroupAffinityMask> masks{{.group = 1, .mask = 0x1}, {.group = 0, .mask = 0x1}};
    EXPECT_EQ(cpuAffinityFromGroupMasks(masks, groups), cpuList("0,64"));
}

TEST(CpuAffinityFromGroupMasksTest, UnevenGroupsOffsetByTheEarlierGroupsSizes)
{
    // 40 + 40 + 16 processors: group 1 starts at CPU 40, group 2 at 80.
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 40, .activeMask = (std::uint64_t{1} << 40U) - 1U},
                                                   {.maximumProcessors = 40, .activeMask = (std::uint64_t{1} << 40U) - 1U},
                                                   {.maximumProcessors = 16, .activeMask = 0xFFFF}};
    const std::vector<GroupAffinityMask> masks{{.group = 0, .mask = 0x3}, {.group = 1, .mask = 0xF}, {.group = 2, .mask = 0x8001}};
    EXPECT_EQ(cpuAffinityFromGroupMasks(masks, groups), cpuList("0-1,40-43,80,95"));
}

TEST(CpuAffinityFromGroupMasksTest, RestrictedToGroupOneOnly)
{
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 64, .activeMask = FULL_GROUP},
                                                   {.maximumProcessors = 64, .activeMask = FULL_GROUP}};
    const std::vector<GroupAffinityMask> masks{{.group = 1, .mask = FULL_GROUP}};
    EXPECT_EQ(cpuAffinityFromGroupMasks(masks, groups), cpuList("64-127"));
    const std::vector<GroupAffinityMask> some{{.group = 1, .mask = 0xF0}};
    EXPECT_EQ(cpuAffinityFromGroupMasks(some, groups), cpuList("68-71"));
}

TEST(CpuAffinityFromGroupMasksTest, GapsWithinAndAcrossGroupsArePreserved)
{
    // Alternate processors, and the high bit of group 0 next to the low bit of group 1.
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 64, .activeMask = FULL_GROUP},
                                                   {.maximumProcessors = 64, .activeMask = FULL_GROUP}};
    const std::vector<GroupAffinityMask> masks{{.group = 0, .mask = 0x5 | (std::uint64_t{1} << 63U)}, {.group = 1, .mask = 0x1 | 0x100}};
    EXPECT_EQ(cpuAffinityFromGroupMasks(masks, groups), cpuList("0,2,63-64,72"));
}

TEST(CpuAffinityFromGroupMasksTest, NumbersByMaximumCountsLeavingRoomForHotAdd)
{
    // Group 0 has room for 64 but 48 are active: group 1 still starts at CPU 64, as the per-core
    // coreIds do (processorGroupFirstCoreIds()).
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 64, .activeMask = (std::uint64_t{1} << 48U) - 1U},
                                                   {.maximumProcessors = 64, .activeMask = FULL_GROUP}};
    const std::vector<GroupAffinityMask> masks{{.group = 0, .mask = (std::uint64_t{1} << 48U) - 1U}, {.group = 1, .mask = 0x3}};
    EXPECT_EQ(cpuAffinityFromGroupMasks(masks, groups), cpuList("0-47,64-65"));
}

TEST(CpuAffinityFromGroupMasksTest, DuplicateGroupsCombineAndInvalidOnesAreDropped)
{
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 8, .activeMask = 0xFF},
                                                   {.maximumProcessors = 8, .activeMask = 0xFF}};
    const std::vector<GroupAffinityMask> masks{{.group = 1, .mask = 0x1},
                                               {.group = 1, .mask = 0x2},
                                               {.group = 0, .mask = 0x100}, // Beyond group 0's 8 processors
                                               {.group = 5, .mask = 0xFF}}; // No such group
    EXPECT_EQ(cpuAffinityFromGroupMasks(masks, groups), cpuList("8-9"));
    EXPECT_TRUE(cpuAffinityFromGroupMasks({}, groups).empty());
    EXPECT_TRUE(cpuAffinityFromGroupMasks(masks, {}).empty());
}

TEST(GroupMasksFromProcessTest, OneGroupTakesTheProcessMask)
{
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 64, .activeMask = FULL_GROUP},
                                                   {.maximumProcessors = 64, .activeMask = FULL_GROUP}};
    const std::vector<std::uint16_t> processGroups{1};
    const auto masks = groupMasksFromProcess(processGroups, 0xF0, groups, true);
    ASSERT_TRUE(masks.has_value());
    EXPECT_EQ(cpuAffinityFromGroupMasks(masks.value_or(std::vector<GroupAffinityMask>{}), groups), cpuList("68-71"));
}

TEST(GroupMasksFromProcessTest, OneFullGroupIsTheDefaultSpanWhereThreadsMaySpan)
{
    // Review #1434: on Windows 11 / Server 2022+ a process that spans every group by default lists
    // only its primary group, with that group's full mask. Read as the span over every group (the
    // default, by far the common case), not as the primary group alone.
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 64, .activeMask = FULL_GROUP},
                                                   {.maximumProcessors = 32, .activeMask = 0xFFFF'FFFF}};
    const std::vector<std::uint16_t> primaryOnly{1};
    const auto spanned = groupMasksFromProcess(primaryOnly, 0xFFFF'FFFF, groups, true);
    ASSERT_TRUE(spanned.has_value());
    EXPECT_EQ(cpuAffinityFromGroupMasks(spanned.value_or(std::vector<GroupAffinityMask>{}), groups), cpuList("0-95"));

    // Before Windows 11 a process runs in one group: its full mask is that group alone.
    const auto confined = groupMasksFromProcess(primaryOnly, 0xFFFF'FFFF, groups, false);
    ASSERT_TRUE(confined.has_value());
    EXPECT_EQ(cpuAffinityFromGroupMasks(confined.value_or(std::vector<GroupAffinityMask>{}), groups), cpuList("64-95"));

    // A restricted mask is exact either way: SetProcessAffinityMask confines it to that group.
    const auto restricted = groupMasksFromProcess(primaryOnly, 0xF, groups, true);
    ASSERT_TRUE(restricted.has_value());
    EXPECT_EQ(cpuAffinityFromGroupMasks(restricted.value_or(std::vector<GroupAffinityMask>{}), groups), cpuList("64-67"));

    // One group on the machine: nothing to span.
    const std::vector<ProcessorGroupLayout> single{{.maximumProcessors = 16, .activeMask = 0xFFFF}};
    const std::vector<std::uint16_t> group0{0};
    const auto only = groupMasksFromProcess(group0, 0xFFFF, single, true);
    ASSERT_TRUE(only.has_value());
    EXPECT_EQ(cpuAffinityFromGroupMasks(only.value_or(std::vector<GroupAffinityMask>{}), single), cpuList("0-15"));
}

TEST(GroupMasksFromProcessTest, DefaultSpanOverEveryGroupNeedsNoThreads)
{
    // Windows 11 / Server 2022+: spans both groups, primary-group mask unrestricted.
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 64, .activeMask = FULL_GROUP},
                                                   {.maximumProcessors = 64, .activeMask = FULL_GROUP}};
    const std::vector<std::uint16_t> processGroups{0, 1};
    const auto masks = groupMasksFromProcess(processGroups, FULL_GROUP, groups, true);
    ASSERT_TRUE(masks.has_value());
    EXPECT_EQ(cpuAffinityFromGroupMasks(masks.value_or(std::vector<GroupAffinityMask>{}), groups), cpuList("0-127"));
}

TEST(GroupMasksFromProcessTest, AmbiguousReadsNeedTheThreads)
{
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 64, .activeMask = FULL_GROUP},
                                                   {.maximumProcessors = 32, .activeMask = 0xFFFF'FFFF}};
    const std::vector<std::uint16_t> both{0, 1};
    // Threads explicitly in several groups: GetProcessAffinityMask reports 0.
    EXPECT_FALSE(groupMasksFromProcess(both, 0, groups, true).has_value());
    // Several groups, and the one mask doesn't match every group's processors: which group is it?
    EXPECT_FALSE(groupMasksFromProcess(both, FULL_GROUP, groups, true).has_value());
    EXPECT_FALSE(groupMasksFromProcess(both, 0xFF, groups, true).has_value());
    // A listed group the layout doesn't have.
    const std::vector<std::uint16_t> unknown{0, 3};
    EXPECT_FALSE(groupMasksFromProcess(unknown, FULL_GROUP, groups, true).has_value());
}

// Two full groups of 64, the layout most of the thread cases use.
std::vector<ProcessorGroupLayout> twoFullGroups()
{
    return {{.maximumProcessors = 64, .activeMask = FULL_GROUP}, {.maximumProcessors = 64, .activeMask = FULL_GROUP}};
}

std::vector<GroupAffinityMask> fromThreads(const std::vector<std::uint16_t>& processGroups,
                                           const std::vector<GroupAffinityMask>& masks,
                                           bool complete,
                                           bool threadsMaySpan)
{
    const ThreadGroupAffinityReads reads{.masks = masks, .complete = complete};
    return groupMasksFromThreads(processGroups, reads, twoFullGroups(), threadsMaySpan);
}

TEST(GroupMasksFromThreadsTest, UnionPerGroupFromTheThreads)
{
    // Two threads in group 0, one in group 1, none with a full mask: exact on any build.
    const std::vector<GroupAffinityMask> threads{{.group = 0, .mask = 0x3}, {.group = 0, .mask = 0xC}, {.group = 1, .mask = 0x1}};
    for (const bool threadsMaySpan : {false, true})
    {
        EXPECT_EQ(cpuAffinityFromGroupMasks(fromThreads({0, 1}, threads, true, threadsMaySpan), twoFullGroups()), cpuList("0-3,64"));
    }
}

TEST(GroupMasksFromThreadsTest, NoReadableThreadIsUnreadable)
{
    EXPECT_TRUE(fromThreads({0, 1}, {}, true, true).empty());
    EXPECT_TRUE(fromThreads({0, 1}, {}, false, false).empty());
}

TEST(GroupMasksFromThreadsTest, AnIncompleteReadIsUnreadable)
{
    // Review #1434: group 1's only (restricted) thread exited or denied access. Its group used to
    // be filled in full, as if spanned by default.
    EXPECT_TRUE(fromThreads({0, 1}, {{.group = 0, .mask = 0xF}}, false, true).empty());
    EXPECT_TRUE(fromThreads({0, 1}, {{.group = 0, .mask = 0xF}}, false, false).empty());
    // Even with every listed group reported, a thread missing may have allowed more processors.
    EXPECT_TRUE(fromThreads({0, 1}, {{.group = 0, .mask = 0xF}, {.group = 1, .mask = 0x1}}, false, false).empty());
}

TEST(GroupMasksFromThreadsTest, AListedGroupNoThreadReportsIsUnreadable)
{
    // Every thread read, yet group 1 is listed with none in it: spanned by default (Windows 11+),
    // or a thread moved between the reads. Not guessed at.
    EXPECT_TRUE(fromThreads({0, 1}, {{.group = 0, .mask = 0xFF}}, true, true).empty());
    EXPECT_TRUE(fromThreads({0, 1}, {{.group = 0, .mask = 0xFF}}, true, false).empty());
}

TEST(GroupMasksFromThreadsTest, AMixedDefaultSpanAndRestrictedThreadIsUnreadable)
{
    // Review #1434: Windows 11+, a default thread reports only its primary group (0, full) while a
    // restricted one reports group 1. The default thread may run on all of group 1 too, so group
    // 1's restricted mask alone is wrong.
    const std::vector<GroupAffinityMask> threads{{.group = 0, .mask = FULL_GROUP}, {.group = 1, .mask = 0xF}};
    EXPECT_TRUE(fromThreads({0, 1}, threads, true, true).empty());
}

TEST(GroupMasksFromThreadsTest, AFullThreadIsItsGroupAloneBeforeWindows11)
{
    // Before Windows 11 / Server 2022 a thread runs in one group, so the same reads are exact.
    const std::vector<GroupAffinityMask> threads{{.group = 0, .mask = FULL_GROUP}, {.group = 1, .mask = 0xF}};
    EXPECT_EQ(cpuAffinityFromGroupMasks(fromThreads({0, 1}, threads, true, false), twoFullGroups()), cpuList("0-67"));
}

TEST(GroupMasksFromThreadsTest, FullThreadsCoveringEveryGroupAgreeEitherWay)
{
    // Bound to its group or spanning every group, the answer is all 128 processors.
    const std::vector<GroupAffinityMask> threads{{.group = 0, .mask = FULL_GROUP}, {.group = 1, .mask = FULL_GROUP}};
    EXPECT_EQ(cpuAffinityFromGroupMasks(fromThreads({0, 1}, threads, true, true), twoFullGroups()), cpuList("0-127"));
}

TEST(GroupMasksFromThreadsTest, AFullThreadWithAnInactiveGroupIsJudgedOnActiveProcessors)
{
    // Group 1 has room for 64 but only 32 active: a thread with all 32 is full there.
    const std::vector<ProcessorGroupLayout> groups{{.maximumProcessors = 64, .activeMask = FULL_GROUP},
                                                   {.maximumProcessors = 64, .activeMask = 0xFFFF'FFFF}};
    const std::vector<std::uint16_t> processGroups{0, 1};
    const ThreadGroupAffinityReads reads{.masks = {{.group = 0, .mask = FULL_GROUP}, {.group = 1, .mask = 0xFFFF'FFFF}}, .complete = true};
    EXPECT_EQ(cpuAffinityFromGroupMasks(groupMasksFromThreads(processGroups, reads, groups, true), groups), cpuList("0-95"));
}

TEST(GroupMasksFromProcessTest, DefaultSpanFollowsTheCurrentActiveMasks)
{
    // Review #1434: the active masks are re-read on the heavy cadence, not fixed at construction.
    // Group 1 hot-adds processors 32-63: a process spanning every group by default now reports
    // the full mask, which only the refreshed layout recognises as the default span.
    const std::vector<std::uint16_t> processGroups{0, 1};
    const std::vector<ProcessorGroupLayout> before{{.maximumProcessors = 64, .activeMask = FULL_GROUP},
                                                   {.maximumProcessors = 64, .activeMask = 0xFFFF'FFFF}};
    const std::vector<ProcessorGroupLayout> after{{.maximumProcessors = 64, .activeMask = FULL_GROUP},
                                                  {.maximumProcessors = 64, .activeMask = FULL_GROUP}};
    EXPECT_FALSE(groupMasksFromProcess(processGroups, FULL_GROUP, before, true).has_value());
    const auto masks = groupMasksFromProcess(processGroups, FULL_GROUP, after, true);
    ASSERT_TRUE(masks.has_value());
    EXPECT_EQ(cpuAffinityFromGroupMasks(masks.value_or(std::vector<GroupAffinityMask>{}), after), cpuList("0-127"));
}

TEST(ThreadsMaySpanGroupsTest, Windows11AndServer2022OnOrUnknown)
{
    EXPECT_FALSE(threadsMaySpanGroups(19045)); // Windows 10 22H2
    EXPECT_FALSE(threadsMaySpanGroups(17763)); // Server 2019
    EXPECT_TRUE(threadsMaySpanGroups(20348));  // Server 2022
    EXPECT_TRUE(threadsMaySpanGroups(22000));  // Windows 11
    EXPECT_TRUE(threadsMaySpanGroups(26100));
    EXPECT_TRUE(threadsMaySpanGroups(0)); // Unknown: the cautious answer
}

TEST(AffinityTopologyTest, FailedDiscoveryIsNeverOneGroup)
{
    // Review #1434: a failed discovery (no groups) used to read as one group, mapping one group's
    // mask onto CPUs 0-63 on a multi-group machine.
    EXPECT_EQ(affinityTopology(0, false), AffinityTopology::Unknown);
    EXPECT_EQ(affinityTopology(0, true), AffinityTopology::Unknown);
    // Several groups whose active masks couldn't be read (at construction or a later refresh).
    EXPECT_EQ(affinityTopology(2, false), AffinityTopology::Unknown);
    EXPECT_EQ(affinityTopology(2, true), AffinityTopology::MultiGroup);
    // One group needs no active masks: read exactly as before #1247.
    EXPECT_EQ(affinityTopology(1, false), AffinityTopology::SingleGroup);
    EXPECT_EQ(affinityTopology(1, true), AffinityTopology::SingleGroup);
}

// ---------------------------------------------------------------------------
// classifyNetworkCounterDenial (#1358): does elevation cure a TCP EStats denial?
// ---------------------------------------------------------------------------

TEST(ClassifyNetworkCounterDenialTest, NonElevatedDenialIsReducedPrivileges)
{
    const NetworkCounterDenial denial = classifyNetworkCounterDenial(/*isElevated=*/false, /*hasNetworkCounters=*/false, true);
    EXPECT_TRUE(denial.reducedPrivileges);
    EXPECT_FALSE(denial.blocked) << "running as Administrator would restore them";
}

TEST(ClassifyNetworkCounterDenialTest, ElevatedDenialIsBlockedNotReducedPrivileges)
{
    // #1358: an elevated session isn't short of privileges, but the columns still need explaining.
    const NetworkCounterDenial denial = classifyNetworkCounterDenial(/*isElevated=*/true, /*hasNetworkCounters=*/false, true);
    EXPECT_TRUE(denial.blocked);
    EXPECT_FALSE(denial.reducedPrivileges);
}

TEST(ClassifyNetworkCounterDenialTest, NoDenialReportsNeither)
{
    for (const bool elevated : {false, true})
    {
        // Working counters, or counters missing for a reason other than access (EStats unsupported).
        for (const bool hasCounters : {false, true})
        {
            const NetworkCounterDenial denial = classifyNetworkCounterDenial(elevated, hasCounters, /*accessDenied=*/false);
            EXPECT_FALSE(denial.reducedPrivileges);
            EXPECT_FALSE(denial.blocked);
        }
        // A denial is never reported while the counters are claimed.
        const NetworkCounterDenial claimed = classifyNetworkCounterDenial(elevated, /*hasNetworkCounters=*/true, true);
        EXPECT_FALSE(claimed.reducedPrivileges);
        EXPECT_FALSE(claimed.blocked);
    }
}

TEST(ClassifyNetworkCounterDenialTest, ElevatedSampleWithEveryEStatsAccessDeniedIsBlocked)
{
    // The probe's path (#1161 + #1358): elevated, the first real sample has every EStats enable and
    // read denied, so it withdraws the counters and records the denial; capabilities() then reports
    // them blocked on this system rather than nothing at all.
    const auto counts = tallyEstablishedRows({
        {.enableStatus = ESTATS_ERROR_ACCESS_DENIED, .readStatus = ESTATS_ERROR_ACCESS_DENIED, .bytesOut = 0, .bytesIn = 0},
        {.enableStatus = ESTATS_ERROR_ACCESS_DENIED, .readStatus = ESTATS_ERROR_ACCESS_DENIED, .bytesOut = 0, .bytesIn = 0},
    });
    ASSERT_EQ(classifyEStatsProbe(counts), EStatsProbeResult::Unavailable);
    const bool accessDenied = counts.accessDenied > 0;
    const NetworkCounterDenial denial = classifyNetworkCounterDenial(/*isElevated=*/true, /*hasNetworkCounters=*/false, accessDenied);
    EXPECT_TRUE(denial.blocked);
    EXPECT_FALSE(denial.reducedPrivileges);
}

} // namespace
} // namespace Platform
