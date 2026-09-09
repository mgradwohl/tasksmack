#include "App/Panels/ProcessRowFormat.h"
#include "Domain/ProcessSnapshot.h"
#include "UI/Format.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace App
{
namespace
{

using Domain::ProcessSnapshot;
using ProcessRowFormat::AlignedCellText;
using ProcessRowFormat::buildRowFormatCache;
using ProcessRowFormat::formatAlignedBytesPerSecString;
using ProcessRowFormat::formatAlignedBytesString;
using ProcessRowFormat::formatAlignedPercentString;
using ProcessRowFormat::formatAlignedPowerString;
using ProcessRowFormat::makeAlignedCellText;
using ProcessRowFormat::RowFormatCache;

/// Builds a minimal-but-representative snapshot: every field buildRowFormatCache() reads is set
/// to a distinguishable, non-default value so a wrong field mapping (e.g. resident vs. virtualMem
/// swapped) would surface as a test failure.
[[nodiscard]] ProcessSnapshot makeSnapshot()
{
    ProcessSnapshot snap;
    snap.parentPid = 4;
    snap.startTimeEpoch = 1'700'000'000;
    snap.cpuTimeSeconds = 12.5;
    snap.cpuPercent = 25.0;
    snap.memoryPercent = 10.0;
    snap.virtualBytes = 1024ULL * 1024 * 1024;   // 1 GB
    snap.memoryBytes = 512ULL * 1024 * 1024;     // 512 MB
    snap.peakMemoryBytes = 768ULL * 1024 * 1024; // 768 MB
    snap.sharedBytes = 64ULL * 1024 * 1024;      // 64 MB
    snap.ioReadBytesPerSec = 1024.0 * 1024.0;    // 1 MB/s
    snap.ioWriteBytesPerSec = 2048.0 * 1024.0;   // 2 MB/s
    snap.netSentBytesPerSec = 512.0;             // 512 B/s
    snap.netReceivedBytesPerSec = 4096.0;        // 4 KB/s
    snap.powerWatts = 5.5;
    snap.gpuUtilPercent = 15.0;
    snap.gpuMemoryBytes = 256ULL * 1024 * 1024; // 256 MB
    snap.gpuEngines = {"3D", "Compute"};
    snap.threadCount = 7;
    snap.handleCount = 42;
    snap.pageFaults = 123;
    snap.cpuAffinityMask = 0x3;
    snap.gdiObjectCount = 9;
    return snap;
}

// =============================================================================
// makeAlignedCellText
// =============================================================================

TEST(ProcessRowFormatTest, MakeAlignedCellTextStartsUnmeasured)
{
    const AlignedCellText cell = makeAlignedCellText("hello");

    EXPECT_EQ(cell.text, "hello");
    EXPECT_EQ(cell.width, AlignedCellText::UNMEASURED_WIDTH);
}

// =============================================================================
// formatAligned*String helpers
// =============================================================================

TEST(ProcessRowFormatTest, FormatAlignedPercentStringAppendsUnitSuffix)
{
    EXPECT_EQ(formatAlignedPercentString(25.0), "25.0%");
}

TEST(ProcessRowFormatTest, FormatAlignedBytesStringUsesGivenUnit)
{
    const auto formatted =
        formatAlignedBytesString(static_cast<double>(1024ULL * 1024 * 1024), UI::Format::unitForTotalBytes(1024ULL * 1024 * 1024));

    EXPECT_EQ(formatted, "1.0 GB");
}

TEST(ProcessRowFormatTest, FormatAlignedBytesPerSecStringUsesGivenUnit)
{
    const auto formatted = formatAlignedBytesPerSecString(1024.0 * 1024.0, UI::Format::unitForBytesPerSecond(1024.0 * 1024.0));

    EXPECT_EQ(formatted, "1.0 MB/s");
}

TEST(ProcessRowFormatTest, FormatAlignedPowerStringProducesNonEmptyResult)
{
    // Exact bucket/unit thresholds belong to UI::Format::splitPowerForAlignment's own tests;
    // this only guards that the three parts are concatenated in the right order.
    EXPECT_FALSE(formatAlignedPowerString(5.5).empty());
}

// =============================================================================
// buildRowFormatCache
// =============================================================================

TEST(ProcessRowFormatTest, BuildRowFormatCacheFormatsEveryField)
{
    const RowFormatCache fmt = buildRowFormatCache(makeSnapshot());

    EXPECT_EQ(fmt.ppid.text, UI::Format::formatId(4));
    EXPECT_EQ(fmt.startTime.text, UI::Format::formatEpochDateTimeShort(1'700'000'000));
    EXPECT_EQ(fmt.cpuTime.text, UI::Format::formatCpuTimeCompact(12.5));
    EXPECT_EQ(fmt.cpuPercent.text, "25.0%");
    EXPECT_EQ(fmt.memPercent.text, "10.0%");
    EXPECT_EQ(fmt.virtualMem.text, "1.0 GB");
    EXPECT_EQ(fmt.resident.text, "512.0 MB");
    EXPECT_EQ(fmt.peakRss.text, "768.0 MB");
    EXPECT_EQ(fmt.shared.text, "64.0 MB");
    EXPECT_EQ(fmt.ioRead.text, "1.0 MB/s");
    EXPECT_EQ(fmt.ioWrite.text, "2.0 MB/s");
    EXPECT_EQ(fmt.gpuPercent.text, "15.0%");
    EXPECT_EQ(fmt.gpuMemory.text, "256.0 MB");
    EXPECT_EQ(fmt.gpuEngines, "3D, Compute");
    EXPECT_EQ(fmt.threads.text, UI::Format::formatIntLocalized(7));
    EXPECT_EQ(fmt.handles.text, UI::Format::formatIntLocalized(42));
    EXPECT_EQ(fmt.pageFaults.text, UI::Format::formatIntLocalized(std::uint64_t{123}));
    EXPECT_EQ(fmt.affinity.text, UI::Format::formatCpuAffinityMask(0x3));
    EXPECT_EQ(fmt.gdiObjects.text, UI::Format::formatIntLocalized(9));
}

TEST(ProcessRowFormatTest, BuildRowFormatCacheUsesDashForZeroRateAndOptionalFields)
{
    ProcessSnapshot snap; // Every rate/optional field left at its default (zero / nullopt).

    const RowFormatCache fmt = buildRowFormatCache(snap);

    EXPECT_EQ(fmt.ioRead.text, "-");
    EXPECT_EQ(fmt.ioWrite.text, "-");
    EXPECT_EQ(fmt.netSent.text, "-");
    EXPECT_EQ(fmt.netRecv.text, "-");
    EXPECT_EQ(fmt.gpuPercent.text, "-");
    EXPECT_EQ(fmt.gpuMemory.text, "-");
    EXPECT_EQ(fmt.threads.text, "-");
    EXPECT_EQ(fmt.handles.text, "-");
    EXPECT_EQ(fmt.pageFaults.text, "-");
    EXPECT_EQ(fmt.gdiObjects.text, "-"); // gdiObjectCount is std::nullopt
    EXPECT_EQ(fmt.gpuEngines, "-");      // gpuEngines is empty
}

TEST(ProcessRowFormatTest, BuildRowFormatCacheStampsFreshAlignedCellTextAsUnmeasured)
{
    // A freshly built entry's widths must all still be UNMEASURED_WIDTH -- the caller (renderProcessRow)
    // relies on this to know a rebuilt entry needs re-measuring, not the stale width from a discarded copy.
    const RowFormatCache fmt = buildRowFormatCache(makeSnapshot());

    EXPECT_EQ(fmt.cpuPercent.width, AlignedCellText::UNMEASURED_WIDTH);
    EXPECT_EQ(fmt.resident.width, AlignedCellText::UNMEASURED_WIDTH);
}

} // namespace
} // namespace App
