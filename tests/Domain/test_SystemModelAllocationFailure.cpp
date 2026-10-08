// SystemModel's history append is a transaction (#1412): a std::bad_alloc anywhere in a sample --
// including one that introduces a new interface and a new core -- must leave every history series
// aligned with the timestamps and the published generation unchanged.
//
// The failure is real, not simulated: AllocationFailureHook makes the Nth allocation the current
// thread makes from now throw std::bad_alloc.

#include "AllocationFailureHook.h"
#include "Domain/SystemModel.h"
#include "Mocks/MockProbes.h"
#include "Platform/SystemTypes.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

using TestSupport::FailAllocationAfter;

namespace
{

constexpr std::uint64_t WARM_UP_SAMPLES = 30;

/// Counters for sample @p step: cores 0..cores-1 and the named interfaces, all advancing.
Platform::SystemCounters countersAt(std::uint64_t step, std::size_t cores, const std::vector<std::string>& interfaces)
{
    const auto cpu = [step](std::uint64_t salt)
    {
        const std::uint64_t busy = 1 + ((step + salt) % 5);
        return TestMocks::makeCpuCounters(step * busy, 0, step, step * (10 - busy));
    };
    std::vector<Platform::CpuCounters> perCore;
    perCore.reserve(cores);
    for (std::size_t core = 0; core < cores; ++core)
    {
        perCore.push_back(cpu(core));
        perCore.back().coreId = core;
    }
    std::vector<Platform::SystemCounters::InterfaceCounters> ifaces;
    ifaces.reserve(interfaces.size());
    for (const auto& name : interfaces)
    {
        ifaces.push_back(TestMocks::makeInterfaceCounters(name, step * 1000, step * 500));
    }
    return TestMocks::makeSystemCounters(
        cpu(99), TestMocks::makeMemoryCounters(1024, 512), step, std::move(perCore), 0, 0, std::move(ifaces));
}

/// Every series of the model's publication is as long as the publication's timestamps, and each
/// interface has both of its series or neither.
void expectAligned(const Domain::SystemModel& model, const std::vector<std::string>& interfaces)
{
    const auto publication = model.publication();
    const std::size_t published = publication->timestamps.size();
    for (const auto* series : {&publication->cpuHistory,
                               &publication->cpuUserHistory,
                               &publication->cpuSystemHistory,
                               &publication->cpuIowaitHistory,
                               &publication->cpuIdleHistory,
                               &publication->memoryHistory,
                               &publication->memoryCachedHistory,
                               &publication->swapHistory,
                               &publication->batteryChargeHistory,
                               &publication->netRxHistory,
                               &publication->netTxHistory})
    {
        EXPECT_EQ(series->size(), published);
    }
    for (const auto& core : publication->perCoreHistory)
    {
        EXPECT_EQ(core.size(), published);
    }
    EXPECT_EQ(publication->perInterfaceRxHistory.size(), publication->perInterfaceTxHistory.size());
    for (const auto& name : interfaces)
    {
        EXPECT_EQ(publication->perInterfaceRxHistory.count(name), publication->perInterfaceTxHistory.count(name)) << name;
    }
    for (const auto& [name, rx] : publication->perInterfaceRxHistory)
    {
        EXPECT_EQ(rx.size(), published) << name;
        EXPECT_EQ(publication->perInterfaceTxHistory.at(name).size(), published) << name;
    }
}

// Fails each allocation of one sample in turn -- the sample that adds core 2 and interface wlan0 to a
// two-core, eth0-only history -- until the sample needs no more. After every failure the series are
// still aligned, the publication and its version are untouched, and the next sample applies cleanly.
TEST(SystemModelAllocationFailureTest, AFailedAllocationOnASampleThatAddsAnInterfaceAndACoreKeepsTheSeriesAligned)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    const std::vector<std::string> before{"eth0"};
    const std::vector<std::string> after{"eth0", "wlan0"};
    constexpr std::int64_t MAX_ALLOCATIONS = 100'000; // a bound on the loop, far above one sample's

    std::int64_t failed = 0;
    for (std::int64_t failAt = 0; failAt < MAX_ALLOCATIONS; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        Domain::SystemModel model(std::make_unique<TestMocks::MockSystemProbe>());
        std::uint64_t step = 0;
        for (; step < WARM_UP_SAMPLES; ++step)
        {
            model.updateFromCounters(countersAt(step, 2, before), static_cast<double>(step));
        }
        const auto publicationBefore = model.publication();
        const std::uint64_t versionBefore = model.publicationVersion();
        const std::size_t samplesBefore = model.publication()->timestamps.size();
        const auto newSample = countersAt(step, 3, after);

        bool threw = false;
        {
            const FailAllocationAfter guard(failAt);
            try
            {
                model.updateFromCounters(newSample, static_cast<double>(step));
            }
            catch (const std::bad_alloc&)
            {
                threw = true;
            }
        }
        if (!threw)
        {
            break; // the sample made fewer than failAt + 1 allocations: every one has been failed
        }
        ++failed;

        EXPECT_EQ(model.publicationVersion(), versionBefore);
        EXPECT_EQ(model.publication(), publicationBefore);

        // The model carries on: the next sample applies and publishes, with the new core and interface.
        // Its publication shows the history the failure left: aligned, and either unmoved by the failed
        // sample or (a failure in publish(), after the append) moved by exactly one sample in every
        // series together.
        model.updateFromCounters(countersAt(step + 1, 3, after), static_cast<double>(step + 1));
        expectAligned(model, after);
        EXPECT_GT(model.publicationVersion(), versionBefore);
        const std::size_t samplesAfter = model.publication()->timestamps.size();
        EXPECT_TRUE(samplesAfter == samplesBefore + 1 || samplesAfter == samplesBefore + 2) << samplesAfter;
        EXPECT_EQ(model.publication()->perCoreHistory.size(), 3U);
        EXPECT_EQ(model.publication()->perInterfaceRxHistory.count("wlan0"), 1U);
        if (HasFailure())
        {
            return; // one failing allocation point is enough to report
        }
    }
    EXPECT_GT(failed, 10); // the sample did allocate, and those allocations were exercised
}

} // namespace
