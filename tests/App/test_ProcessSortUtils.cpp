#include "App/Panels/ProcessSortUtils.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace App
{
namespace
{

using Domain::ProcessSnapshot;

/// Builds a snapshot whose fields are all "low" (or all "high", via `high`) so that a
/// single fixture pair exercises every column's ordering with one comparison.
[[nodiscard]] ProcessSnapshot makeSnapshot(bool high)
{
    ProcessSnapshot snap;
    const std::int32_t i = high ? 2 : 1;
    const double d = high ? 2.0 : 1.0;
    const std::uint64_t u = high ? 200 : 100;
    const std::string s = high ? "b" : "a";

    snap.pid = i;
    snap.parentPid = i;
    snap.nice = i;
    snap.threadCount = i;
    snap.handleCount = i;

    snap.cpuPercent = d;
    snap.memoryPercent = d;
    snap.cpuTimeSeconds = d;

    snap.memoryBytes = u;
    snap.virtualBytes = u;
    snap.startTimeEpoch = u;
    snap.uniqueKey = u;

    snap.ioReadBytesPerSec = d;
    snap.ioWriteBytesPerSec = d;
    snap.netSentBytesPerSec = d;
    snap.netReceivedBytesPerSec = d;
    snap.powerWatts = d;

    snap.peakMemoryBytes = u;
    snap.sharedBytes = u;
    snap.pageFaults = u;
    snap.cpuAffinityMask = u;

    snap.gpuUtilPercent = d;
    snap.gpuMemoryBytes = u;

    snap.gdiObjectCount = i;

    snap.name = s;
    snap.command = s;
    snap.user = s;
    snap.displayState = s;
    snap.status = s;
    snap.gpuDevices = s;
    snap.publisher = s;
    snap.processType = s;

    // Same size (1) for both, so the generic per-column loop below exercises the
    // "compare first engine name" branch, not the "compare engine count" branch -
    // that one gets its own dedicated test.
    snap.gpuEngines = {s};

    return snap;
}

// =============================================================================
// Every column: ascending/descending ordering
// =============================================================================

TEST(ProcessSortUtilsTest, LowSortsBeforeHighAscendingForEveryColumn)
{
    const ProcessSnapshot low = makeSnapshot(false);
    const ProcessSnapshot high = makeSnapshot(true);

    for (const ProcessColumn column : allProcessColumns())
    {
        SCOPED_TRACE(static_cast<int>(column));
        EXPECT_TRUE(ProcessSortUtils::compareByColumn(low, high, column, /*ascending=*/true));
        EXPECT_FALSE(ProcessSortUtils::compareByColumn(high, low, column, /*ascending=*/true));
    }
}

TEST(ProcessSortUtilsTest, HighSortsBeforeLowDescendingForEveryColumn)
{
    const ProcessSnapshot low = makeSnapshot(false);
    const ProcessSnapshot high = makeSnapshot(true);

    for (const ProcessColumn column : allProcessColumns())
    {
        SCOPED_TRACE(static_cast<int>(column));
        EXPECT_TRUE(ProcessSortUtils::compareByColumn(high, low, column, /*ascending=*/false));
        EXPECT_FALSE(ProcessSortUtils::compareByColumn(low, high, column, /*ascending=*/false));
    }
}

TEST(ProcessSortUtilsTest, EqualSnapshotsNeverCompareLessForAnyColumn)
{
    // A strict-weak-ordering comparator must return false when both sides are equal,
    // in both directions - std::ranges::sort relies on this to terminate correctly.
    const ProcessSnapshot a = makeSnapshot(false);
    const ProcessSnapshot b = makeSnapshot(false);

    for (const ProcessColumn column : allProcessColumns())
    {
        SCOPED_TRACE(static_cast<int>(column));
        EXPECT_FALSE(ProcessSortUtils::compareByColumn(a, b, column, true));
        EXPECT_FALSE(ProcessSortUtils::compareByColumn(a, b, column, false));
    }
}

// =============================================================================
// Column-specific edge cases not covered by the generic low/high pair
// =============================================================================

TEST(ProcessSortUtilsTest, GpuEngineComparesByCountBeforeFirstName)
{
    ProcessSnapshot fewer;
    fewer.gpuEngines = {"z"}; // lexically greater name, but fewer engines

    ProcessSnapshot more;
    more.gpuEngines = {"a", "b"}; // lexically lesser first name, but more engines

    // Ascending: fewer engines sorts first, regardless of engine names.
    EXPECT_TRUE(ProcessSortUtils::compareByColumn(fewer, more, ProcessColumn::GpuEngine, true));
    EXPECT_FALSE(ProcessSortUtils::compareByColumn(more, fewer, ProcessColumn::GpuEngine, true));
}

TEST(ProcessSortUtilsTest, GpuEngineWithNoEnginesIsNotLessThanItself)
{
    ProcessSnapshot none;
    ProcessSnapshot alsoNone;

    EXPECT_FALSE(ProcessSortUtils::compareByColumn(none, alsoNone, ProcessColumn::GpuEngine, true));
    EXPECT_FALSE(ProcessSortUtils::compareByColumn(none, alsoNone, ProcessColumn::GpuEngine, false));
}

TEST(ProcessSortUtilsTest, GdiObjectsUnavailableSortsBeforeAvailable)
{
    // gdiObjectCount is std::optional<int32_t>; std::nullopt means the probe couldn't open the
    // process, distinct from a genuine value of 0. std::optional::operator< treats nullopt as
    // less than any engaged value.
    ProcessSnapshot unavailable; // gdiObjectCount left at std::nullopt
    ProcessSnapshot available;
    available.gdiObjectCount = 0;

    EXPECT_TRUE(ProcessSortUtils::compareByColumn(unavailable, available, ProcessColumn::GdiObjects, true));
    EXPECT_FALSE(ProcessSortUtils::compareByColumn(available, unavailable, ProcessColumn::GdiObjects, true));
}

// =============================================================================
// Tie-breaker: equal keys order by PID, then unique key (#1174)
// =============================================================================

TEST(ProcessSortUtilsTest, EqualKeysOrderByPidAscending)
{
    ProcessSnapshot lowPid = makeSnapshot(false);
    ProcessSnapshot highPid = makeSnapshot(false); // Every key equal...
    highPid.pid = 50;                              // ...but the PID
    lowPid.pid = 7;

    for (const ProcessColumn column : allProcessColumns())
    {
        if (column == ProcessColumn::PID)
        {
            continue;
        }
        SCOPED_TRACE(static_cast<int>(column));
        EXPECT_TRUE(ProcessSortUtils::compareByColumn(lowPid, highPid, column, /*ascending=*/true));
        EXPECT_FALSE(ProcessSortUtils::compareByColumn(highPid, lowPid, column, /*ascending=*/true));
    }
}

TEST(ProcessSortUtilsTest, EqualKeysOrderByPidDescending)
{
    ProcessSnapshot lowPid = makeSnapshot(false);
    ProcessSnapshot highPid = makeSnapshot(false);
    highPid.pid = 50;
    lowPid.pid = 7;

    for (const ProcessColumn column : allProcessColumns())
    {
        if (column == ProcessColumn::PID)
        {
            continue;
        }
        SCOPED_TRACE(static_cast<int>(column));
        EXPECT_TRUE(ProcessSortUtils::compareByColumn(highPid, lowPid, column, /*ascending=*/false));
        EXPECT_FALSE(ProcessSortUtils::compareByColumn(lowPid, highPid, column, /*ascending=*/false));
    }
}

TEST(ProcessSortUtilsTest, EqualKeysAndPidOrderByUniqueKey)
{
    ProcessSnapshot older = makeSnapshot(false);
    ProcessSnapshot newer = makeSnapshot(false); // Same PID, reused by a later process
    older.uniqueKey = 1;
    newer.uniqueKey = 2;

    EXPECT_TRUE(ProcessSortUtils::compareByColumn(older, newer, ProcessColumn::CpuPercent, true));
    EXPECT_FALSE(ProcessSortUtils::compareByColumn(newer, older, ProcessColumn::CpuPercent, true));
    EXPECT_TRUE(ProcessSortUtils::compareByColumn(newer, older, ProcessColumn::CpuPercent, false));
    EXPECT_FALSE(ProcessSortUtils::compareByColumn(older, newer, ProcessColumn::CpuPercent, false));
}

TEST(ProcessSortUtilsTest, SortOfTiedRowsIsTheSameWhateverTheInputOrder)
{
    // Most processes tie at 0.0% CPU; the sort must give one order however the rows arrive, or
    // they reshuffle on every refresh (#1174).
    std::vector<ProcessSnapshot> rows;
    for (const std::int32_t pid : {40, 3, 17, 8, 25, 1, 12})
    {
        ProcessSnapshot snap;
        snap.pid = pid;
        snap.cpuPercent = (pid == 17) ? 5.0 : 0.0;
        rows.push_back(snap);
    }

    const auto sortedPids = [](std::vector<ProcessSnapshot> input, bool ascending)
    {
        std::ranges::sort(input,
                          [ascending](const ProcessSnapshot& a, const ProcessSnapshot& b)
                          { return ProcessSortUtils::compareByColumn(a, b, ProcessColumn::CpuPercent, ascending); });
        std::vector<std::int32_t> pids;
        pids.reserve(input.size());
        for (const auto& snap : input)
        {
            pids.push_back(snap.pid);
        }
        return pids;
    };

    const std::vector<std::int32_t> expectedDescending = {17, 40, 25, 12, 8, 3, 1};
    const std::vector<std::int32_t> expectedAscending = {1, 3, 8, 12, 25, 40, 17};

    auto shuffled = rows;
    for (int pass = 0; pass < 4; ++pass)
    {
        SCOPED_TRACE(pass);
        EXPECT_EQ(sortedPids(shuffled, false), expectedDescending);
        EXPECT_EQ(sortedPids(shuffled, true), expectedAscending);
        std::ranges::rotate(shuffled, shuffled.begin() + 3);
        std::ranges::reverse(shuffled);
    }
}

TEST(ProcessSortUtilsTest, UnreadableValuesSortBelowEveryReadingIncludingZero)
{
    // #1110: without root, another user's FD count, I/O and network rates can't be read. They sort
    // below every reading -- a real 0 included -- instead of mixing in with the processes that read 0.
    ProcessSnapshot unreadable;
    unreadable.handleCount = 0;
    unreadable.handleCountAvailable = false;
    unreadable.ioAvailable = false;
    unreadable.networkAvailable = false;
    const ProcessSnapshot zero; // every value read, and 0

    for (const ProcessColumn column :
         {ProcessColumn::Handles, ProcessColumn::IoRead, ProcessColumn::IoWrite, ProcessColumn::NetSent, ProcessColumn::NetReceived})
    {
        EXPECT_TRUE(ProcessSortUtils::compareByColumn(unreadable, zero, column, true)) << static_cast<int>(column);
        EXPECT_FALSE(ProcessSortUtils::compareByColumn(zero, unreadable, column, true)) << static_cast<int>(column);
        EXPECT_TRUE(ProcessSortUtils::compareByColumn(zero, unreadable, column, false)) << static_cast<int>(column);
    }
}

TEST(ProcessSortUtilsTest, UnknownColumnReturnsFalse)
{
    const ProcessSnapshot low = makeSnapshot(false);
    const ProcessSnapshot high = makeSnapshot(true);

    // ProcessColumn::Count is the enum's sentinel, not a real column; compareByColumn should
    // fall through to its default branch rather than reading an out-of-range field.
    EXPECT_FALSE(ProcessSortUtils::compareByColumn(low, high, ProcessColumn::Count, true));
    EXPECT_FALSE(ProcessSortUtils::compareByColumn(high, low, ProcessColumn::Count, true));
}

} // namespace
} // namespace App
