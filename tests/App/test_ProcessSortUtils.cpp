#include "App/Panels/ProcessSortUtils.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"

#include <gtest/gtest.h>

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
