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
