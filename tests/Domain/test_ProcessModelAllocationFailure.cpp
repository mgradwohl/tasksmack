/// @file test_ProcessModelAllocationFailure.cpp
/// @brief ProcessModel's GPU merge under a failed allocation (#1142, #1891): a std::bad_alloc while
/// merging the per-process GPU publication must not escape refresh() -- the processes are published
/// without GPU fields -- and the next refresh merges again.
///
/// The failure is real, not simulated: AllocationFailureHook makes the Nth allocation the current thread
/// makes from now throw std::bad_alloc. Allocations elsewhere in refresh() may propagate; the test steps
/// the failing allocation through the whole refresh and checks every outcome that returned.

#include "AllocationFailureHook.h"
#include "Domain/GPUModel.h"
#include "Domain/ProcessModel.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/GPUTypes.h"

#include <gtest/gtest.h>

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

} // namespace
