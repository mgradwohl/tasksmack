/// @file test_PidIndex.cpp
/// @brief Domain::PidIndex (#2981): ProcessModel's reusable PID -> row table for the process tree.
/// The last row wins for a repeated PID, a rebuild forgets the previous one, and a rebuild that fits
/// the table it has allocates nothing.

#include "AllocationFailureHook.h"
#include "Domain/PidIndex.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <vector>

namespace Domain
{
namespace
{

[[nodiscard]] std::vector<std::int32_t> sequentialPids(std::size_t count, std::int32_t first)
{
    std::vector<std::int32_t> pids;
    pids.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        pids.push_back(first + static_cast<std::int32_t>(i));
    }
    return pids;
}

TEST(PidIndexTest, FindsEachPidsRow)
{
    PidIndex index;
    const std::vector<std::int32_t> pids{4, 8, 12, 1000, 0};
    index.rebuild(pids, std::identity{});

    for (std::size_t row = 0; row < pids.size(); ++row)
    {
        EXPECT_EQ(index.find(pids[row]), row) << "pid " << pids[row];
    }
    EXPECT_EQ(index.find(5), PidIndex::NOT_FOUND);
    EXPECT_EQ(index.find(-1), PidIndex::NOT_FOUND);
}

TEST(PidIndexTest, FindBeforeAnyRebuildIsNotFound)
{
    const PidIndex index;
    EXPECT_EQ(index.find(1), PidIndex::NOT_FOUND);
    EXPECT_EQ(index.activeSlots(), 0U);
}

// std::unordered_map::operator[] assignment, which this replaces, kept the last row for a PID listed
// more than once; ProcessModel's tree links children to that row.
TEST(PidIndexTest, RepeatedPidMapsToItsLastRow)
{
    PidIndex index;
    const std::vector<std::int32_t> pids{7, 3, 7, 9, 7};
    index.rebuild(pids, std::identity{});

    EXPECT_EQ(index.find(7), 4U);
    EXPECT_EQ(index.find(3), 1U);
    EXPECT_EQ(index.find(9), 3U);
}

TEST(PidIndexTest, RebuildForgetsThePreviousRows)
{
    PidIndex index;
    index.rebuild(std::vector<std::int32_t>{1, 2, 3}, std::identity{});
    index.rebuild(std::vector<std::int32_t>{3, 4}, std::identity{});

    EXPECT_EQ(index.find(1), PidIndex::NOT_FOUND);
    EXPECT_EQ(index.find(2), PidIndex::NOT_FOUND);
    EXPECT_EQ(index.find(3), 0U);
    EXPECT_EQ(index.find(4), 1U);

    index.rebuild(std::vector<std::int32_t>{}, std::identity{});
    EXPECT_EQ(index.find(3), PidIndex::NOT_FOUND);
}

// Windows PIDs are multiples of 4; colliding low bits must not break lookups.
TEST(PidIndexTest, FindsEveryPidOfALargeSet)
{
    PidIndex index;
    constexpr std::int32_t COUNT = 10'000;
    std::vector<std::int32_t> pids;
    pids.reserve(COUNT);
    for (std::int32_t i = 0; i < COUNT; ++i)
    {
        pids.push_back(i * 4);
    }
    index.rebuild(pids, std::identity{});

    for (std::size_t row = 0; row < pids.size(); ++row)
    {
        ASSERT_EQ(index.find(pids[row]), row) << "pid " << pids[row];
    }
    EXPECT_EQ(index.find(2), PidIndex::NOT_FOUND);
    EXPECT_EQ(index.find(40'000), PidIndex::NOT_FOUND);
    EXPECT_GE(index.activeSlots(), pids.size() * 2);
}

TEST(PidIndexTest, ReadsThePidThroughTheProjection)
{
    struct Row
    {
        std::int32_t pid = 0;
        std::int32_t parentPid = 0;
    };
    PidIndex index;
    const std::vector<Row> rows{{.pid = 10, .parentPid = 1}, {.pid = 20, .parentPid = 10}};
    index.rebuild(rows, &Row::pid);

    EXPECT_EQ(index.find(10), 0U);
    EXPECT_EQ(index.find(20), 1U);
    EXPECT_EQ(index.find(1), PidIndex::NOT_FOUND);
}

// A stable process set keeps its table: the second refresh's rebuild reuses the slots.
TEST(PidIndexTest, RebuildOfTheSameSizeKeepsItsTable)
{
    PidIndex index;
    const auto pids = sequentialPids(1'000, 100);
    index.rebuild(pids, std::identity{});
    const std::size_t capacity = index.capacity();

    index.rebuild(pids, std::identity{});
    EXPECT_EQ(index.capacity(), capacity);
    EXPECT_EQ(index.find(100), 0U);
    EXPECT_EQ(index.find(1'099), 999U);
}

// Bounded retention: a spike in the process count does not keep its large table forever.
TEST(PidIndexTest, ShrinksAfterTheProcessCountFalls)
{
    PidIndex index;
    index.rebuild(sequentialPids(10'000, 1), std::identity{});
    const std::size_t large = index.capacity();

    // Small drops reuse the table.
    index.rebuild(sequentialPids(5'000, 1), std::identity{});
    EXPECT_EQ(index.capacity(), large);

    index.rebuild(sequentialPids(100, 1), std::identity{});
    EXPECT_LT(index.capacity(), large);
    EXPECT_LE(index.capacity(), PidIndex::SHRINK_FACTOR * index.activeSlots());
    EXPECT_EQ(index.find(100), 99U);
    EXPECT_EQ(index.find(101), PidIndex::NOT_FOUND);
}

// The #2981 acceptance check: rebuilding an unchanged 1,000-process set allocates nothing (any
// allocation would throw the armed failure).
TEST(PidIndexTest, SecondRebuildOfAnUnchangedSetDoesNotAllocate)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#else
    PidIndex index;
    const auto pids = sequentialPids(1'000, 100);
    index.rebuild(pids, std::identity{});
    {
        const TestSupport::FailAllocationAfter guard(0);
        EXPECT_NO_THROW(index.rebuild(pids, std::identity{}));
        EXPECT_TRUE(TestSupport::allocationFailurePending()) << "rebuild() allocated";
    }
    EXPECT_EQ(index.find(1'099), 999U);
#endif
}

// A failed growth leaves the table empty, never answering with the previous build's rows.
TEST(PidIndexTest, FailedGrowthLeavesTheTableEmpty)
{
#if defined(TASKSMACK_NO_ALLOCATOR_HOOK)
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#else
    PidIndex index;
    index.rebuild(std::vector<std::int32_t>{1, 2, 3}, std::identity{});
    const auto larger = sequentialPids(1'000, 1);
    {
        const TestSupport::FailAllocationAfter guard(0);
        EXPECT_THROW(index.rebuild(larger, std::identity{}), std::bad_alloc);
    }
    EXPECT_EQ(index.find(1), PidIndex::NOT_FOUND);
    EXPECT_EQ(index.activeSlots(), 0U);

    index.rebuild(larger, std::identity{});
    EXPECT_EQ(index.find(1), 0U);
    EXPECT_EQ(index.find(1'000), 999U);
#endif
}

} // namespace
} // namespace Domain
