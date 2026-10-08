// GPUModel's history append is a transaction (#1412): a std::bad_alloc anywhere in a refresh --
// including one where a rescan adds a GPU and another drops out of the read -- must leave every GPU's
// series aligned with its timestamps and the published generation whole.
//
// The failure is real, not simulated: AllocationFailureHook makes the Nth allocation the current
// thread makes from now throw std::bad_alloc. GPUModel::refreshAt() catches and logs it, so the test
// asks the hook whether the failure fired rather than catching it.

#include "AllocationFailureHook.h"
#include "Domain/GPUModel.h"
#include "Mocks/MockGPUProbe.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

using TestMocks::MockGPUProbe;
using TestSupport::FailAllocationAfter;

namespace
{

constexpr int WARM_UP_SAMPLES = 30;

/// Every series of every GPU in the model's publication is as long as its published timestamps.
void expectAligned(const Domain::GPUModel& model)
{
    const auto publication = model.publication();
    ASSERT_NE(publication, nullptr);
    for (const auto& [gpuId, history] : publication->histories)
    {
        const std::size_t samples = history.timestamps.size();
        EXPECT_EQ(history.memoryUsedBytes.size(), samples) << gpuId;
        EXPECT_EQ(history.memoryTotalBytes.size(), samples) << gpuId;
        for (const auto* series : {&history.utilization,
                                   &history.memoryPercent,
                                   &history.gpuClock,
                                   &history.encoder,
                                   &history.decoder,
                                   &history.temperature,
                                   &history.power,
                                   &history.fanSpeed})
        {
            EXPECT_EQ(series->size(), samples) << gpuId;
        }
    }
}

// Fails each allocation of one refresh in turn -- the one where a rescan lists a new GPU2, which the
// read reports for the first time, and GPU1 drops out of the read (so it gets a placeholder, #1146) --
// until the refresh needs no more. After every failure each GPU's series are still aligned, the
// publication is either untouched or the whole new generation, and the next refresh applies cleanly.
TEST(GPUModelAllocationFailureTest, AFailedAllocationOnARefreshThatAddsAGpuKeepsTheSeriesAligned)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    constexpr std::int64_t MAX_ALLOCATIONS = 100'000; // a bound on the loop, far above one refresh's
    const auto start = std::chrono::ceil<std::chrono::seconds>(std::chrono::steady_clock::now());

    std::int64_t failed = 0;
    for (std::int64_t failAt = 0; failAt < MAX_ALLOCATIONS; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        auto probe = std::make_unique<MockGPUProbe>();
        auto* rawProbe = probe.get();
        rawProbe->withGPU("GPU0", "GPU Zero").withGPU("GPU1", "GPU One");
        Domain::GPUModel model(std::move(probe));
        int step = 0;
        for (; step < WARM_UP_SAMPLES; ++step)
        {
            rawProbe->withUtilization("GPU0", static_cast<double>(step)).withUtilization("GPU1", static_cast<double>(step * 2));
            model.refreshAt(start + std::chrono::seconds(step));
        }
        const auto publicationBefore = model.publication();
        const std::uint64_t versionBefore = model.publicationVersion();
        const std::size_t samplesBefore = publicationBefore->histories.at("GPU0").timestamps.size();

        rawProbe->withGPU("GPU2", "GPU Two").withUtilization("GPU2", 55.0).withoutGPUCounters("GPU1").withRescanReportingChange();
        bool fired = false;
        {
            const FailAllocationAfter guard(failAt);
            model.refreshAt(start + std::chrono::seconds(step));
            fired = !TestSupport::allocationFailurePending();
        }
        if (!fired)
        {
            break; // the refresh made fewer than failAt + 1 allocations: every one has been failed
        }
        ++failed;

        expectAligned(model);
        // Nothing is published (a failure before or in publish()), or the whole generation is (one after it).
        if (model.publicationVersion() == versionBefore)
        {
            EXPECT_EQ(model.publication(), publicationBefore);
        }
        else
        {
            EXPECT_EQ(model.publicationVersion(), versionBefore + 1);
            EXPECT_TRUE(model.publication()->histories.contains("GPU2"));
        }

        // The model carries on: the next refresh applies and publishes GPU2, and GPU1's gap. Its
        // publication shows the history the failure left: either unmoved by the failed refresh, or
        // moved by exactly that sample for every GPU together -- GPU2's history has it only if the
        // sample applied.
        ++step;
        model.refreshAt(start + std::chrono::seconds(step));
        expectAligned(model);
        EXPECT_GT(model.publicationVersion(), versionBefore);
        const auto next = model.publication();
        ASSERT_TRUE(next->histories.contains("GPU0"));
        const std::size_t samplesAfter = next->histories.at("GPU0").timestamps.size();
        EXPECT_TRUE(samplesAfter == samplesBefore + 1 || samplesAfter == samplesBefore + 2) << samplesAfter;
        ASSERT_TRUE(next->histories.contains("GPU2"));
        EXPECT_EQ(next->histories.at("GPU2").timestamps.size(), samplesAfter - samplesBefore);
        EXPECT_FLOAT_EQ(next->histories.at("GPU2").utilization.back(), 55.0F);
        ASSERT_TRUE(next->histories.contains("GPU1"));
        EXPECT_EQ(next->histories.at("GPU1").timestamps.size(), samplesAfter);
        EXPECT_TRUE(std::isnan(next->histories.at("GPU1").utilization.back()));
        if (HasFailure())
        {
            return; // one failing allocation point is enough to report
        }
    }
    EXPECT_GT(failed, 10); // the refresh did allocate, and those allocations were exercised
}

} // namespace
