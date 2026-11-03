/// @file test_ProcessModelAllocationFailure.cpp
/// @brief ProcessModel's GPU merge under a failed allocation (#1142, #1891): a std::bad_alloc while
/// merging the per-process GPU publication must not escape refresh() -- the processes are published
/// without GPU fields -- and the next refresh merges again. And its aggregated system histories
/// (#2156): a failed append leaves the five series aligned, and a history-window change never allocates.
///
/// The failure is real, not simulated: AllocationFailureHook makes the Nth allocation the current thread
/// makes from now throw std::bad_alloc. Allocations elsewhere in refresh() may propagate; the test steps
/// the failing allocation through the whole refresh and checks every outcome that returned.

#include "AllocationFailureHook.h"
#include "Domain/GPUModel.h"
#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/GPUTypes.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

using TestMocks::MockGPUProbe;
using TestMocks::MockProcessProbe;
using TestSupport::FailAllocationAfter;

namespace
{

constexpr std::uint64_t GPU_MEMORY = 512ULL * 1024 * 1024;

TEST(ProcessModelAllocationFailureTest, AFailedGpuMergeKeepsPublishingAndTheNextRefreshMergesAgain)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    constexpr std::int64_t MAX_ALLOCATIONS = 100'000; // a bound on the loop, far above one refresh's
    int mergeFailures = 0;
    for (std::int64_t failAt = 0; failAt < MAX_ALLOCATIONS; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        auto processProbe = std::make_unique<MockProcessProbe>();
        processProbe->setCounters({TestMocks::makeProcessCounters(100, "server", 'R', 1000, 500, 1000, std::uint64_t{1024} * 1024, 1)});
        processProbe->setTotalCpuTime(100000);
        auto gpuProbe = std::make_unique<MockGPUProbe>();
        Platform::GPUCapabilities caps;
        caps.hasPerProcessMetrics = true;
        gpuProbe->withCapabilities(caps);
        gpuProbe->withGPU("GPU0", "Test GPU", "TestVendor").withProcessGPU(100, "GPU0", GPU_MEMORY);
        auto gpuModel = std::make_shared<Domain::GPUModel>(std::move(gpuProbe));
        Domain::ProcessModel model(std::move(processProbe));
        model.setGPUModel(gpuModel);
        model.refresh(); // seed: the processes are listed before the GPU read, as in the app (#1417)
        gpuModel->refresh();

        bool threw = false;
        bool fired = false;
        {
            const FailAllocationAfter guard(failAt);
            try
            {
                model.refresh();
            }
            catch (const std::bad_alloc&)
            {
                threw = true; // a failure outside the merge: refresh() may let it through
            }
            fired = !TestSupport::allocationFailurePending();
        }
        if (!fired)
        {
            break; // the refresh made fewer than failAt + 1 allocations: every one has been failed
        }
        if (threw)
        {
            continue;
        }
        const auto failed = model.snapshots();
        if (failed.size() == 1 && failed[0].gpuMemoryBytes == 0)
        {
            ++mergeFailures; // published without GPU fields: the merge's own guard caught it
            // ... and the next refresh merges again.
            model.refresh();
            const auto recovered = model.snapshots();
            ASSERT_EQ(recovered.size(), 1U);
            EXPECT_EQ(recovered[0].gpuMemoryBytes, GPU_MEMORY);
        }
    }
    EXPECT_GT(mergeFailures, 0) << "some failing allocation lands in the GPU merge";
}

/// The model's published aggregated system histories (all empty before the first generation).
Domain::ProcessSystemHistories systemHistories(const Domain::ProcessModel& model)
{
    Domain::ProcessSystemHistories histories;
    static_cast<void>(model.tryCopySystemHistoriesIfNewer(0, histories));
    return histories;
}

/// The number of samples in the five aggregated system histories, which must all be the same.
std::size_t alignedHistoryLength(const Domain::ProcessModel& model)
{
    const auto histories = systemHistories(model);
    const std::size_t length = histories.timestamps.size();
    EXPECT_EQ(histories.power.size(), length);
    EXPECT_EQ(histories.pageFaults.size(), length);
    EXPECT_EQ(histories.threadCount.size(), length);
    EXPECT_EQ(histories.handleCount.size(), length);
    return length;
}

/// Drives a ProcessModel with no probe through updateFromCounters() on a manual clock, one second a sample.
class HistoryDriver
{
  public:
    HistoryDriver() : m_Model(nullptr, [this] { return m_Now; })
    {}

    void sample()
    {
        m_Now += std::chrono::seconds(1);
        m_TotalCpu += 100000;
        m_Counter.userTime += 100;
        m_Model.updateFromCounters({m_Counter}, m_TotalCpu);
    }

    Domain::ProcessModel& model()
    {
        return m_Model;
    }

  private:
    Domain::ProcessModel::Clock::time_point m_Now;
    std::uint64_t m_TotalCpu = 100000;
    Platform::ProcessCounters m_Counter = TestMocks::makeProcessCounters(100, "server", 'R', 1000, 500, 1000, 1024, 1);
    Domain::ProcessModel m_Model;
};

// #2156: the five aggregated system histories are appended as one transaction. Fails each allocation
// of a refresh that has to move the histories to a bigger shared block, in turn: afterwards the five
// series are always the same length -- the sample appended to all of them or to none -- and the next
// refresh appends to all five again.
TEST(ProcessModelAllocationFailureTest, AFailedSystemHistoryAppendKeepsTheFiveSeriesAligned)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    // The seed plus enough history samples to fill a series' first shared block (64 samples), so the
    // refresh under test compacts every series into a new one.
    constexpr int WARM_UP_SAMPLES = 1 + 64;
    constexpr std::int64_t MAX_ALLOCATIONS = 100'000; // a bound on the loop, far above one refresh's
    int containedFailures = 0;
    for (std::int64_t failAt = 0; failAt < MAX_ALLOCATIONS; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        HistoryDriver driver;
        for (int i = 0; i < WARM_UP_SAMPLES; ++i)
        {
            driver.sample();
        }
        const std::size_t before = alignedHistoryLength(driver.model());
        ASSERT_EQ(before, static_cast<std::size_t>(WARM_UP_SAMPLES - 1));

        bool threw = false;
        bool fired = false;
        {
            const FailAllocationAfter guard(failAt);
            try
            {
                driver.sample();
            }
            catch (const std::bad_alloc&)
            {
                threw = true; // a failure outside the history append: refresh may let it through
            }
            fired = !TestSupport::allocationFailurePending();
        }
        if (!fired)
        {
            break; // the refresh made fewer than failAt + 1 allocations: every one has been failed
        }

        const std::size_t after = alignedHistoryLength(driver.model());
        EXPECT_TRUE(after == before || after == before + 1) << after;
        if (!threw && after == before)
        {
            ++containedFailures; // the append failed and was contained: the process list was still published
        }

        // The model carries on: the next refresh appends to all five series.
        driver.sample();
        EXPECT_GT(alignedHistoryLength(driver.model()), after);
        if (HasFailure())
        {
            return; // one failing allocation point is enough to report
        }
    }
    EXPECT_GT(containedFailures, 0) << "some failing allocation lands in the history append";
}

// #3334: a refresh that fails after rebuilding the PID -> row table for its reordered rows must not leave the
// next refresh's held-over lookup reading the still-published rows through it. A and B share a start tick (same
// clock tick), so only the PID tells them apart. Each allocation of the reordered refresh is failed in turn; after
// any failure, a refresh with A held over and B read publishes A's own row and B's, and B under A in the tree.
TEST(ProcessModelAllocationFailureTest, AFailedRefreshLeavesNoHeldOverRowOnAnotherProcess)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    constexpr std::uint64_t SAME_TICK = 1234;
    constexpr std::int64_t MAX_ALLOCATIONS = 100'000; // a bound on the loop, far above one refresh's
    const Platform::ProcessCounters a = TestMocks::makeProcessCounters(100, "parent", 'S', 100, 50, SAME_TICK, 1024, 1);
    const Platform::ProcessCounters b = TestMocks::makeProcessCounters(200, "child", 'S', 100, 50, SAME_TICK, 1024, 100);
    int failedRefreshes = 0;
    for (std::int64_t failAt = 0; failAt < MAX_ALLOCATIONS; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        Domain::ProcessModel::Clock::time_point now;
        Domain::ProcessModel model(nullptr, [&now] { return now; });
        std::uint64_t total = 1000;
        const auto sample = [&](std::vector<Platform::ProcessCounters> counters)
        {
            now += std::chrono::seconds(1);
            total += 1000;
            model.updateFromCounters(counters, total);
        };
        sample({a, b});

        bool threw = false;
        bool fired = false;
        {
            const FailAllocationAfter guard(failAt);
            try
            {
                sample({b, a}); // reordered: the rows the table is rebuilt for swap places
            }
            catch (const std::bad_alloc&)
            {
                threw = true;
            }
            fired = !TestSupport::allocationFailurePending();
        }
        if (!fired)
        {
            break; // the refresh made fewer than failAt + 1 allocations: every one has been failed
        }
        failedRefreshes += threw ? 1 : 0;

        // A carried record says nothing reliable but its identity: whatever else it holds, A's row is its last one.
        Platform::ProcessCounters heldA = a;
        heldA.heldOver = true;
        heldA.name = "from the carried record";
        sample({heldA, b});
        const auto rows = model.snapshots();
        ASSERT_EQ(rows.size(), 2U);
        EXPECT_EQ(rows[0].pid, 100);
        EXPECT_EQ(rows[0].name, "parent") << "A's own last row: not B's, and not rebuilt from the carried record";
        EXPECT_EQ(rows[1].pid, 200);
        EXPECT_EQ(rows[0].childrenIndices, (std::vector<std::size_t>{1})) << "B is under A";
        if (HasFailure())
        {
            return; // one failing allocation point is enough to report
        }
    }
    EXPECT_GT(failedRefreshes, 0) << "some failing allocation throws out of the refresh";
}

// #3334: the same, with a PID listed twice (reused within one read) and the held-over refresh's own allocations
// failed in turn too, so the lookup's table rebuild fails and it walks the rows: the held-over process still gets
// its own row, not the other one on its PID.
TEST(ProcessModelAllocationFailureTest, AHeldOverLookupThatCantRebuildItsTableFindsItsOwnRowOnAReusedPid)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    constexpr std::int64_t MAX_ALLOCATIONS = 100'000;
    const Platform::ProcessCounters first = TestMocks::makeProcessCounters(100, "first", 'S', 100, 50, 10, 1024, 1);
    const Platform::ProcessCounters second = TestMocks::makeProcessCounters(100, "second", 'S', 100, 50, 20, 1024, 1);
    // Enough others that the failed refresh's table is far bigger: rebuilding it for two rows then shrinks it, which
    // allocates, so failing that allocation sends the lookup down its walk.
    std::vector<Platform::ProcessCounters> many{first, second};
    for (std::int32_t pid = 300; pid < 430; ++pid) // 132 rows: a 256-slot table, more than 8x the 16 two rows need
    {
        many.push_back(TestMocks::makeProcessCounters(pid, "other", 'S', 100, 50, 30, 1024, 1));
    }
    int walkedLookups = 0;
    for (std::int64_t failRefresh = 0; failRefresh < MAX_ALLOCATIONS; ++failRefresh)
    {
        bool refreshFired = false;
        for (std::int64_t failLookup = 0; failLookup < MAX_ALLOCATIONS; ++failLookup)
        {
            SCOPED_TRACE("failing allocation " + std::to_string(failRefresh) + ", then " + std::to_string(failLookup));
            Domain::ProcessModel::Clock::time_point now;
            Domain::ProcessModel model(nullptr, [&now] { return now; });
            std::uint64_t total = 1000;
            const auto sample = [&](std::vector<Platform::ProcessCounters> counters)
            {
                now += std::chrono::seconds(1);
                total += 1000;
                model.updateFromCounters(counters, total);
            };
            sample({first, second});
            bool threw = false;
            {
                const FailAllocationAfter guard(failRefresh);
                try
                {
                    sample(many); // the table is rebuilt for many more rows, then the refresh fails
                }
                catch (const std::bad_alloc&)
                {
                    threw = true;
                }
                refreshFired = !TestSupport::allocationFailurePending();
            }
            if (!refreshFired || !threw)
            {
                break; // every failing point of this refresh has been tried, or this one didn't fail it
            }
            Platform::ProcessCounters heldSecond = second;
            heldSecond.heldOver = true;
            heldSecond.name = "from the carried record";
            std::vector<Platform::ProcessCounters> next{first, heldSecond};
            bool lookupThrew = false;
            bool lookupFired = false;
            {
                const FailAllocationAfter guard(failLookup);
                try
                {
                    sample(std::move(next));
                }
                catch (const std::bad_alloc&)
                {
                    lookupThrew = true;
                }
                lookupFired = !TestSupport::allocationFailurePending();
            }
            if (!lookupFired)
            {
                break;
            }
            if (lookupThrew)
            {
                continue;
            }
            ++walkedLookups;
            const auto rows = model.snapshots();
            ASSERT_EQ(rows.size(), 2U);
            EXPECT_EQ(rows[1].startTimeTicks, 20U);
            EXPECT_EQ(rows[1].name, "second") << "its own last row, not the other one on PID 100";
            if (HasFailure())
            {
                return;
            }
        }
        if (!refreshFired)
        {
            break;
        }
    }
    EXPECT_GT(walkedLookups, 0) << "some held-over refresh survives a failed allocation";
}

// #2156: changing the history window never allocates for the histories, so it cannot fail part way
// and leave the five series with different capacities or lengths.
TEST(ProcessModelAllocationFailureTest, ChangingTheHistoryWindowMakesNoAllocation)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    HistoryDriver driver;
    for (int i = 0; i < 40; ++i)
    {
        driver.sample();
    }
    ASSERT_EQ(alignedHistoryLength(driver.model()), 39U);

    for (const int seconds :
         {Domain::Sampling::HISTORY_SECONDS_MAX, Domain::Sampling::HISTORY_SECONDS_MIN, Domain::Sampling::HISTORY_SECONDS_DEFAULT})
    {
        SCOPED_TRACE("history seconds " + std::to_string(seconds));
        {
            const FailAllocationAfter guard(0);
            EXPECT_NO_THROW(driver.model().setMaxHistorySeconds(seconds));
            EXPECT_TRUE(TestSupport::allocationFailurePending()) << "setMaxHistorySeconds() allocated";
        }
        static_cast<void>(alignedHistoryLength(driver.model()));
    }
    // HISTORY_SECONDS_MIN trimmed the histories to t = 30..40 plus the anchor at t = 29 (#1016).
    EXPECT_EQ(alignedHistoryLength(driver.model()), 12U);
}

} // namespace
