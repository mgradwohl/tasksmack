// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
// Tests for benchmarks/AllocationCounter.h: the allocator-side telemetry behind TaskSmackMemoryManager (#879).
#include "AllocationCounter.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace BenchmarkUtils
{
namespace
{

TEST(AllocationCounterTest, StartsAtZero)
{
    const AllocationCounter counter;
    const auto totals = counter.snapshot();

    EXPECT_EQ(totals.allocationCount, 0U);
    EXPECT_EQ(totals.deallocationCount, 0U);
    EXPECT_EQ(totals.bytesAllocated, 0U);
    EXPECT_EQ(totals.bytesDeallocated, 0U);
    EXPECT_EQ(totals.liveBytes, 0);
    EXPECT_EQ(totals.peakLiveBytes, 0);
}

TEST(AllocationCounterTest, CountsAllocationsAndDeallocationsSeparately)
{
    AllocationCounter counter;
    counter.recordAllocation(100);
    counter.recordAllocation(50);
    counter.recordDeallocation(100);

    EXPECT_EQ(counter.allocationCount(), 2U);
    EXPECT_EQ(counter.deallocationCount(), 1U);
    EXPECT_EQ(counter.bytesAllocated(), 150U);
    EXPECT_EQ(counter.bytesDeallocated(), 100U);
    EXPECT_EQ(counter.liveBytes(), 50);
}

// The #879 bug: max_bytes_used was fed the cumulative bytes allocated, so a loop that allocates and frees
// the same 64 bytes a thousand times reported 64000 bytes "used" when no more than 64 were ever live.
TEST(AllocationCounterTest, PeakLiveBytesIsNotCumulativeBytesAllocated)
{
    AllocationCounter counter;
    for (int i = 0; i < 1000; ++i)
    {
        counter.recordAllocation(64);
        counter.recordDeallocation(64);
    }

    EXPECT_EQ(counter.bytesAllocated(), 64000U);
    EXPECT_EQ(counter.peakLiveBytes(), 64);
    EXPECT_EQ(counter.liveBytes(), 0);
}

TEST(AllocationCounterTest, PeakLiveBytesTracksHighWaterMark)
{
    AllocationCounter counter;
    counter.recordAllocation(100);   // live 100
    counter.recordAllocation(200);   // live 300 <- peak
    counter.recordDeallocation(200); // live 100
    counter.recordAllocation(150);   // live 250, below the earlier peak

    EXPECT_EQ(counter.liveBytes(), 250);
    EXPECT_EQ(counter.peakLiveBytes(), 300);

    counter.recordAllocation(100); // live 350, new peak
    EXPECT_EQ(counter.peakLiveBytes(), 350);
}

TEST(AllocationCounterTest, ResetClearsEverything)
{
    AllocationCounter counter;
    counter.recordAllocation(500);
    counter.recordDeallocation(200);
    counter.reset();

    const auto totals = counter.snapshot();
    EXPECT_EQ(totals.allocationCount, 0U);
    EXPECT_EQ(totals.deallocationCount, 0U);
    EXPECT_EQ(totals.bytesAllocated, 0U);
    EXPECT_EQ(totals.bytesDeallocated, 0U);
    EXPECT_EQ(totals.liveBytes, 0);
    EXPECT_EQ(totals.peakLiveBytes, 0);
}

// Memory allocated before a reset and freed after it shows as negative live bytes (net heap shrinkage
// since the reset) rather than wrapping an unsigned total, and the peak stays at the reset baseline.
TEST(AllocationCounterTest, FreeingPreResetMemoryGoesNegativeWithoutRaisingPeak)
{
    AllocationCounter counter;
    counter.recordAllocation(1000);
    counter.reset();
    counter.recordDeallocation(1000);

    EXPECT_EQ(counter.liveBytes(), -1000);
    EXPECT_EQ(counter.peakLiveBytes(), 0);

    counter.recordAllocation(400); // live -600: still below the reset baseline
    EXPECT_EQ(counter.peakLiveBytes(), 0);
}

TEST(AllocationCounterTest, SnapshotMatchesAccessors)
{
    AllocationCounter counter;
    counter.recordAllocation(10);
    counter.recordAllocation(20);
    counter.recordDeallocation(10);

    const auto totals = counter.snapshot();
    EXPECT_EQ(totals.allocationCount, counter.allocationCount());
    EXPECT_EQ(totals.deallocationCount, counter.deallocationCount());
    EXPECT_EQ(totals.bytesAllocated, counter.bytesAllocated());
    EXPECT_EQ(totals.bytesDeallocated, counter.bytesDeallocated());
    EXPECT_EQ(totals.liveBytes, counter.liveBytes());
    EXPECT_EQ(totals.peakLiveBytes, counter.peakLiveBytes());
}

// Every thread holds at most one block at a time, so the peak can never exceed threads * blockSize, and
// once every thread has freed its block the live total returns to exactly zero.
TEST(AllocationCounterTest, ConcurrentRecordingKeepsTotalsConsistent)
{
    constexpr int kThreads = 8;
    constexpr int kIterations = 10000;
    constexpr std::size_t kBlockSize = 32;

    AllocationCounter counter;
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t)
        {
            threads.emplace_back(
                [&counter]
                {
                    for (int i = 0; i < kIterations; ++i)
                    {
                        counter.recordAllocation(kBlockSize);
                        counter.recordDeallocation(kBlockSize);
                    }
                });
        }
    }

    constexpr auto kTotalOps = static_cast<std::uint64_t>(kThreads) * kIterations;
    EXPECT_EQ(counter.allocationCount(), kTotalOps);
    EXPECT_EQ(counter.deallocationCount(), kTotalOps);
    EXPECT_EQ(counter.bytesAllocated(), kTotalOps * kBlockSize);
    EXPECT_EQ(counter.liveBytes(), 0);
    EXPECT_GE(counter.peakLiveBytes(), static_cast<std::int64_t>(kBlockSize));
    EXPECT_LE(counter.peakLiveBytes(), static_cast<std::int64_t>(kThreads * kBlockSize));
}

TEST(AllocationCounterTest, InstanceIsAProcessWideSingleton)
{
    EXPECT_EQ(&AllocationCounter::instance(), &AllocationCounter::instance());
}

} // namespace
} // namespace BenchmarkUtils
// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
