// StorageModel's sample is a transaction (#1412): a std::bad_alloc anywhere in a sample -- including
// one that introduces a new disk -- must leave every history series aligned with the timestamps, the
// published generation unchanged, and the per-disk rate state as it was.
//
// The failure is real, not simulated: AllocationFailureHook makes the Nth allocation the current
// thread makes from now throw (see AllocationFailureHook.h).

#include "AllocationFailureHook.h"
#include "Domain/StorageModel.h"
#include "Mocks/MockDiskProbe.h"
#include "Platform/StorageTypes.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr std::uint64_t WARM_UP_SAMPLES = 30;

/// Counters for sample @p step of the named disks, all advancing.
Platform::SystemDiskCounters countersAt(std::uint64_t step, const std::vector<std::string>& disks)
{
    Platform::SystemDiskCounters counters;
    counters.disks.reserve(disks.size());
    std::uint64_t salt = 0;
    for (const auto& name : disks)
    {
        Platform::DiskCounters disk;
        disk.deviceName = name;
        disk.sectorSize = 512;
        disk.readsCompleted = step * (2 + salt);
        disk.readSectors = step * (16 + salt);
        disk.writesCompleted = step;
        disk.writeSectors = step * (8 + salt);
        counters.disks.push_back(disk);
        ++salt;
    }
    return counters;
}

std::chrono::steady_clock::time_point timeAt(std::uint64_t step)
{
    return std::chrono::steady_clock::time_point{} + std::chrono::hours(1) + std::chrono::seconds(step);
}

/// Every series of the model's publication is as long as the publication's timestamps.
void expectAligned(const Domain::StorageModel& model)
{
    const auto publication = model.publication();
    const std::size_t published = publication->timestamps.size();
    EXPECT_EQ(publication->totalReadHistory.size(), published);
    EXPECT_EQ(publication->totalWriteHistory.size(), published);
    for (const auto& disk : publication->perDiskHistory)
    {
        EXPECT_EQ(disk.readBytesPerSec.size(), published) << disk.deviceName;
        EXPECT_EQ(disk.writeBytesPerSec.size(), published) << disk.deviceName;
    }
}

// Fails each allocation of one sample in turn -- the sample that adds nvme0n1 to an sda-only history
// -- until the sample needs no more. After every failure the series are still aligned, the
// publication and its version are untouched, and the next sample applies cleanly and measures rates
// for sda as if the failed sample had never happened.
TEST(StorageModelAllocationFailureTest, AFailedAllocationOnASampleThatAddsADiskKeepsTheSeriesAligned)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#endif
    const std::vector<std::string> before{"sda"};
    const std::vector<std::string> after{"sda", "nvme0n1"};
    constexpr std::int64_t MAX_ALLOCATIONS = 100'000; // a bound on the loop, far above one sample's

    std::int64_t failed = 0;
    for (std::int64_t failAt = 0; failAt < MAX_ALLOCATIONS; ++failAt)
    {
        SCOPED_TRACE("failing allocation " + std::to_string(failAt));
        auto probe = std::make_unique<Mocks::MockDiskProbe>();
        auto* rawProbe = probe.get();
        Domain::StorageModel model(std::move(probe));
        std::uint64_t step = 0;
        for (; step < WARM_UP_SAMPLES; ++step)
        {
            rawProbe->setNextCounters(countersAt(step, before));
            model.sampleAt(timeAt(step));
        }
        const auto publicationBefore = model.publication();
        const std::uint64_t versionBefore = model.publicationVersion();
        const std::size_t samplesBefore = model.publication()->timestamps.size();
        rawProbe->setNextCounters(countersAt(step, after));

        bool threw = false;
        {
            const TestSupport::FailAllocationAfter guard(failAt);
            try
            {
                model.sampleAt(timeAt(step));
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

        // The model carries on: the next sample applies and publishes, with the new disk. Its
        // publication shows the history the failure left: aligned, and either unmoved by the failed
        // sample or (a failure in publish(), after the append) moved by exactly one sample in every
        // series together.
        ++step;
        rawProbe->setNextCounters(countersAt(step, after));
        model.sampleAt(timeAt(step));
        expectAligned(model);
        EXPECT_GT(model.publicationVersion(), versionBefore);
        const auto publication = model.publication();
        const std::size_t samplesAfter = publication->timestamps.size();
        EXPECT_TRUE(samplesAfter == samplesBefore + 1 || samplesAfter == samplesBefore + 2) << samplesAfter;
        ASSERT_EQ(publication->perDiskHistory.size(), 2U);
        EXPECT_EQ(publication->perDiskHistory[1].deviceName, "nvme0n1");
        // sda's rate is measured against whichever sample was last applied: one interval's I/O (16
        // sectors a second) whether or not the failed sample's state was kept -- never a sample
        // counted twice or a rate over a failed sample's interval with its counters missing.
        const auto snapshot = model.latestSnapshot();
        ASSERT_EQ(snapshot.disks.size(), 2U);
        EXPECT_DOUBLE_EQ(snapshot.disks[0].readBytesPerSec, 16.0 * 512.0);
        if (HasFailure())
        {
            return; // one failing allocation point is enough to report
        }
    }
    EXPECT_GT(failed, 10); // the sample did allocate, and those allocations were exercised
}

} // namespace
