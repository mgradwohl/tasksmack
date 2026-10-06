#include "App/Panels/ProcessRowFormat.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/CpuAffinity.h"
#include "UI/Format.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <locale>
#include <string>
#include <tuple>
#include <unordered_map>

namespace App
{
namespace
{

using Domain::ProcessSnapshot;
using ProcessRowFormat::alignedBytesCell;
using ProcessRowFormat::alignedBytesPerSecCell;
using ProcessRowFormat::AlignedCellText;
using ProcessRowFormat::alignedPowerCell;
using ProcessRowFormat::buildRowFormatCache;
using ProcessRowFormat::formatAlignedBytesPerSecString;
using ProcessRowFormat::formatAlignedBytesString;
using ProcessRowFormat::formatAlignedPercentString;
using ProcessRowFormat::formatAlignedPowerString;
using ProcessRowFormat::getOrBuildRowFormatCache;
using ProcessRowFormat::makeAlignedCellText;
using ProcessRowFormat::RowFormatCache;
using ProcessRowFormat::UNAVAILABLE_CELL_TEXT;

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
    snap.cpuAffinity = Platform::CpuAffinity::fromMask(0x3);
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
    EXPECT_FLOAT_EQ(cell.width, AlignedCellText::UNMEASURED_WIDTH);
}

// =============================================================================
// formatAligned*String helpers
// =============================================================================

TEST(ProcessRowFormatTest, FormatAlignedPercentStringAppendsUnitSuffix)
{
    EXPECT_EQ(formatAlignedPercentString(25.0), "25.0%");
}

// #1195 review: Process Details shows the same percents as the Processes table, so the two must
// never disagree -- not at a rounding boundary such as 6.25, and not in the decimal separator.
TEST(ProcessRowFormatTest, ProcessDetailsPercentMatchesTheTableEverywhere)
{
    for (int hundredths = 0; hundredths <= 10'000; hundredths += 5)
    {
        const double percent = hundredths / 100.0;
        EXPECT_EQ(UI::Format::percentOneDecimal(percent), formatAlignedPercentString(percent)) << "at " << percent;
    }
}

/// A decimal comma and no digit grouping, as in de_DE, without depending on an OS locale name.
class CommaDecimalNumpunct : public std::numpunct<char>
{
  protected:
    [[nodiscard]] char do_decimal_point() const override
    {
        return ',';
    }
};

/// Makes a comma-decimal locale global for one test and restores the previous one after it.
class ScopedCommaDecimalLocale
{
  public:
    // std::locale takes ownership of the facet and deletes it with its last copy, which the analyzer
    // does not see.
    // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks,cppcoreguidelines-owning-memory)
    ScopedCommaDecimalLocale() : m_Previous(std::locale::global(std::locale(std::locale::classic(), new CommaDecimalNumpunct)))
    {}
    // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks,cppcoreguidelines-owning-memory)
    ~ScopedCommaDecimalLocale()
    {
        std::locale::global(m_Previous);
    }
    ScopedCommaDecimalLocale(const ScopedCommaDecimalLocale&) = delete;
    ScopedCommaDecimalLocale& operator=(const ScopedCommaDecimalLocale&) = delete;
    ScopedCommaDecimalLocale(ScopedCommaDecimalLocale&&) = delete;
    ScopedCommaDecimalLocale& operator=(ScopedCommaDecimalLocale&&) = delete;

  private:
    std::locale m_Previous;
};

// #1202: the table's aligned cells and the shared value formatters (tooltips, Process Details, chart
// axes) print the same decimal separator in a comma-decimal locale, not "1.5 MiB" beside "1,5 MiB".
TEST(ProcessRowFormatTest, TableCellsUseTheLocaleDecimalPointLikeTheValueFormatters)
{
    const ScopedCommaDecimalLocale commaLocale;
    const double bytes = 1.5 * 1024.0 * 1024.0;

    EXPECT_EQ(formatAlignedBytesString(bytes, UI::Format::BYTE_UNIT_MB), "1,5 MiB");
    EXPECT_EQ(UI::Format::formatBytes(bytes), "1,5 MiB");
    EXPECT_EQ(formatAlignedBytesPerSecString(bytes, UI::Format::BYTE_UNIT_MB), "1,5 MiB/s");
    EXPECT_EQ(UI::Format::formatBytesPerSec(bytes), "1,5 MiB/s");

    EXPECT_EQ(formatAlignedPercentString(0.6), "0,6%");
    EXPECT_EQ(UI::Format::formatPercent(0.6), "0,6%");
    EXPECT_EQ(UI::Format::percentOneDecimal(0.6), "0,6%");

    EXPECT_EQ(formatAlignedPowerString(45.0), "45,0 W");
    EXPECT_EQ(UI::Format::formatWatts(45.0), "45,0 W");
    EXPECT_EQ(formatAlignedPowerString(0.0), "0,0 W");
    EXPECT_EQ(UI::Format::formatPowerOrZero(0.0), "0,0 W");

    // The allocating slow path agrees with the per-cell fast path.
    const auto slow = UI::Format::splitBytesForAlignment(bytes, UI::Format::BYTE_UNIT_MB);
    EXPECT_EQ(slow.wholePart + slow.decimalPart + slow.unitPart, "1,5 MiB");

    for (int hundredths = 0; hundredths <= 10'000; hundredths += 5)
    {
        const double percent = hundredths / 100.0;
        EXPECT_EQ(UI::Format::percentOneDecimal(percent), formatAlignedPercentString(percent)) << "at " << percent;
    }
}

TEST(ProcessRowFormatTest, FormatAlignedBytesStringUsesGivenUnit)
{
    const auto formatted =
        formatAlignedBytesString(static_cast<double>(1024ULL * 1024 * 1024), UI::Format::unitForTotalBytes(1024ULL * 1024 * 1024));

    EXPECT_EQ(formatted, "1.0 GiB");
}

TEST(ProcessRowFormatTest, FormatAlignedBytesPerSecStringUsesGivenUnit)
{
    const auto formatted = formatAlignedBytesPerSecString(1024.0 * 1024.0, UI::Format::unitForBytesPerSecond(1024.0 * 1024.0));

    EXPECT_EQ(formatted, "1.0 MiB/s");
}

TEST(ProcessRowFormatTest, FormatAlignedPowerStringConcatenatesPartsInOrder)
{
    EXPECT_EQ(formatAlignedPowerString(5.5), "5.5 W");
}

// =============================================================================
// buildRowFormatCache
// =============================================================================

TEST(ProcessRowFormatTest, BuildRowFormatCacheFormatsEveryField)
{
    const RowFormatCache fmt = buildRowFormatCache(makeSnapshot());

    EXPECT_EQ(fmt.ppid.text, UI::Format::formatId(4));
    EXPECT_EQ(fmt.startTime.text, UI::Format::formatEpochDateTimeShort(1'700'000'000));
    EXPECT_EQ(fmt.cpuTime.text, UI::Format::formatDuration(12.5));
    EXPECT_EQ(fmt.cpuPercent.text, "25.0%");
    EXPECT_EQ(fmt.memPercent.text, "10.0%");
    EXPECT_EQ(fmt.virtualMem.text, "1.0 GiB");
    EXPECT_EQ(fmt.resident.text, "512.0 MiB");
    EXPECT_EQ(fmt.peakRss.text, "768.0 MiB");
    EXPECT_EQ(fmt.shared.text, "64.0 MiB");
    EXPECT_EQ(fmt.ioRead.text, "1.0 MiB/s");
    EXPECT_EQ(fmt.ioWrite.text, "2.0 MiB/s");
    EXPECT_EQ(fmt.netSent.text, "512.0 B/s");
    EXPECT_EQ(fmt.netRecv.text, "4.0 KiB/s");
    EXPECT_EQ(fmt.power.text, "5.5 W");
    EXPECT_EQ(fmt.gpuPercent.text, "15.0%");
    EXPECT_EQ(fmt.gpuMemory.text, "256.0 MiB");
    EXPECT_EQ(fmt.gpuEngines, "3D, Compute");
    EXPECT_EQ(fmt.threads.text, UI::Format::formatIntLocalized(7));
    EXPECT_EQ(fmt.handles.text, UI::Format::formatIntLocalized(42));
    EXPECT_EQ(fmt.pageFaults.text, UI::Format::formatIntLocalized(std::uint64_t{123}));
    EXPECT_EQ(fmt.affinity.text, "0,1");
    EXPECT_EQ(fmt.gdiObjects.text, UI::Format::formatIntLocalized(9));
}

TEST(ProcessRowFormatTest, FieldsThePlatformDoesNotFillReadAsDash)
{
    // Windows measures neither power (#1028) nor shared memory (#1035): those cells must say "no
    // data", not a measured-looking value, even when a snapshot carries one.
    const RowFormatCache filled = buildRowFormatCache(makeSnapshot());
    const RowFormatCache unfilled = buildRowFormatCache(makeSnapshot(), {.hasPowerUsage = false, .hasSharedMemory = false});

    EXPECT_EQ(filled.power.text, "5.5 W");
    EXPECT_EQ(filled.shared.text, "64.0 MiB");
    EXPECT_EQ(unfilled.power.text, "-");
    EXPECT_EQ(unfilled.shared.text, "-");

    std::unordered_map<std::uint64_t, RowFormatCache> cache;
    const RowFormatCache& entry = getOrBuildRowFormatCache(cache, makeSnapshot(), 1, 1, {.hasPowerUsage = false, .hasSharedMemory = true});
    EXPECT_EQ(entry.power.text, "-");
    EXPECT_EQ(entry.shared.text, "64.0 MiB");
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

TEST(ProcessRowFormatTest, UnreadableValuesShowNotAvailableRatherThanADash)
{
    // #1110: without root, another user's FD count, I/O and network rates can't be read. They showed
    // "-", the same as a process that really had none; now they read "N/A".
    ProcessSnapshot snap = makeSnapshot();
    snap.handleCountAvailable = false;
    snap.ioAvailable = false;
    snap.networkAvailable = false;

    const RowFormatCache fmt = buildRowFormatCache(snap);

    EXPECT_EQ(fmt.handles.text, UNAVAILABLE_CELL_TEXT);
    EXPECT_EQ(fmt.ioRead.text, UNAVAILABLE_CELL_TEXT);
    EXPECT_EQ(fmt.ioWrite.text, UNAVAILABLE_CELL_TEXT);
    EXPECT_EQ(fmt.netSent.text, UNAVAILABLE_CELL_TEXT);
    EXPECT_EQ(fmt.netRecv.text, UNAVAILABLE_CELL_TEXT);
    EXPECT_NE(UNAVAILABLE_CELL_TEXT, "-");
}

TEST(ProcessRowFormatTest, BuildRowFormatCacheStampsFreshAlignedCellTextAsUnmeasured)
{
    // A freshly built entry's widths must all still be UNMEASURED_WIDTH -- the caller (renderProcessRow)
    // relies on this to know a rebuilt entry needs re-measuring, not the stale width from a discarded copy.
    const RowFormatCache fmt = buildRowFormatCache(makeSnapshot());

    EXPECT_FLOAT_EQ(fmt.cpuPercent.width, AlignedCellText::UNMEASURED_WIDTH);
    EXPECT_FLOAT_EQ(fmt.resident.width, AlignedCellText::UNMEASURED_WIDTH);
}

// =============================================================================
// getOrBuildRowFormatCache -- the lazy get-or-build policy itself, not just the formatting
// buildRowFormatCache() produces. ImGui-free (only touches the map), so first access, same-
// generation reuse, and generation/font invalidation are all directly testable here rather than
// only indirectly through renderProcessRow()'s live ImGui context.
// =============================================================================

TEST(ProcessRowFormatTest, GetOrBuildRowFormatCacheBuildsOnFirstAccess)
{
    std::unordered_map<std::uint64_t, RowFormatCache> cache;
    ProcessSnapshot snap = makeSnapshot();
    snap.uniqueKey = 1;

    const RowFormatCache& fmt = getOrBuildRowFormatCache(cache, snap, /*generation=*/1, /*fontId=*/0);

    EXPECT_EQ(fmt.cpuPercent.text, "25.0%");
    EXPECT_EQ(fmt.generation, 1U);
    EXPECT_EQ(fmt.fontId, 0U);
}

TEST(ProcessRowFormatTest, GetOrBuildRowFormatCacheBuildsOnFirstAccessWithZeroGenerationAndZeroFontId)
{
    // generation == 0 / fontId == 0 are legal stamp values that also happen to match a
    // default-constructed RowFormatCache, so a stamp comparison alone can't distinguish "never
    // built" from "already built for exactly these stamps". A get-or-build that only compares
    // stamps would return an empty, unformatted entry here and render a row of blank cells.
    std::unordered_map<std::uint64_t, RowFormatCache> cache;
    ProcessSnapshot snap = makeSnapshot();
    snap.uniqueKey = 1;

    const RowFormatCache& fmt = getOrBuildRowFormatCache(cache, snap, /*generation=*/0, /*fontId=*/0);

    EXPECT_EQ(fmt.cpuPercent.text, "25.0%");
    EXPECT_EQ(fmt.generation, 0U);
    EXPECT_EQ(fmt.fontId, 0U);
}

TEST(ProcessRowFormatTest, GetOrBuildRowFormatCacheReusesEntryForSameGenerationAndFont)
{
    std::unordered_map<std::uint64_t, RowFormatCache> cache;
    ProcessSnapshot snapA = makeSnapshot();
    snapA.uniqueKey = 1;
    getOrBuildRowFormatCache(cache, snapA, /*generation=*/1, /*fontId=*/0);

    // A second call for the same key/generation/font, with a snapshot whose data has since
    // changed (cpuPercent differs), must reuse the existing entry rather than rebuilding: a
    // regression that always rebuilds would defeat the entire point of this cache and would
    // still pass every buildRowFormatCache() test, since those never call this function twice.
    ProcessSnapshot snapB = makeSnapshot();
    snapB.uniqueKey = 1;
    snapB.cpuPercent = 99.0;
    const RowFormatCache& fmt = getOrBuildRowFormatCache(cache, snapB, /*generation=*/1, /*fontId=*/0);

    EXPECT_EQ(fmt.cpuPercent.text, "25.0%"); // Still snapA's value -- not rebuilt.
}

TEST(ProcessRowFormatTest, GetOrBuildRowFormatCacheRebuildsWhenGenerationAdvances)
{
    std::unordered_map<std::uint64_t, RowFormatCache> cache;
    ProcessSnapshot snapA = makeSnapshot();
    snapA.uniqueKey = 1;
    getOrBuildRowFormatCache(cache, snapA, /*generation=*/1, /*fontId=*/0);

    ProcessSnapshot snapB = makeSnapshot();
    snapB.uniqueKey = 1;
    snapB.cpuPercent = 99.0;
    const RowFormatCache& fmt = getOrBuildRowFormatCache(cache, snapB, /*generation=*/2, /*fontId=*/0);

    EXPECT_EQ(fmt.cpuPercent.text, "99.0%");
    EXPECT_EQ(fmt.generation, 2U);
}

TEST(ProcessRowFormatTest, GetOrBuildRowFormatCacheRebuildsWhenFontChangesWithoutAGenerationBump)
{
    // A font/size/DPI change alone, with no new data version, must still force a rebuild -- an
    // AlignedCellText's cached width was measured under the old font and would otherwise stay
    // wrong until the next data refresh happens to land.
    std::unordered_map<std::uint64_t, RowFormatCache> cache;
    ProcessSnapshot snap = makeSnapshot();
    snap.uniqueKey = 1;
    // Two distinct font identities. These are plain integer tokens, not addresses of locals:
    // the stamp is a std::uintptr_t precisely so no caller address is stored in the cache
    // (CodeQL cpp/stack-address-escape, see #904).
    constexpr std::uintptr_t oldFontToken = 0xF001;
    constexpr std::uintptr_t newFontToken = 0xF002;
    getOrBuildRowFormatCache(cache, snap, /*generation=*/1, /*fontId=*/oldFontToken);

    // Simulate the old entry's width having already been measured by a prior render.
    cache.at(1).cpuPercent.width = 42.0F;

    const RowFormatCache& fmt = getOrBuildRowFormatCache(cache, snap, /*generation=*/1, /*fontId=*/newFontToken);

    EXPECT_FLOAT_EQ(fmt.cpuPercent.width, AlignedCellText::UNMEASURED_WIDTH); // Rebuilt, not reusing the stale width.
    EXPECT_EQ(fmt.fontId, newFontToken);
}

// =============================================================================
// Free-text cell widths (#1141)
// =============================================================================

TEST(ProcessRowFormatTest, LazyTextWidthMeasuresOnceThenReusesTheWidth)
{
    const ProcessRowFormat::LazyTextWidth width;
    int measurements = 0;
    const auto measure = [&measurements]
    {
        ++measurements;
        return 123.5F;
    };
    EXPECT_FLOAT_EQ(width.get(measure), 123.5F);
    EXPECT_FLOAT_EQ(width.get(measure), 123.5F); // a later frame: no glyph lookups
    EXPECT_EQ(measurements, 1);
}

TEST(ProcessRowFormatTest, FreeTextWidthsAreMeasuredAgainForANewGenerationOrFont)
{
    // The cached widths of the name/user/command cells live in the row's entry, so they are dropped
    // exactly when the text can change (a new snapshot generation) or its width can (a new font).
    std::unordered_map<std::uint64_t, RowFormatCache> cache;
    const ProcessSnapshot proc = makeSnapshot();
    int measurements = 0;
    const auto measure = [&measurements]
    {
        ++measurements;
        return 10.0F;
    };

    std::ignore = getOrBuildRowFormatCache(cache, proc, 1, 1).commandWidth.get(measure);
    std::ignore = getOrBuildRowFormatCache(cache, proc, 1, 1).commandWidth.get(measure);
    EXPECT_EQ(measurements, 1);

    std::ignore = getOrBuildRowFormatCache(cache, proc, 2, 1).commandWidth.get(measure); // new snapshot
    EXPECT_EQ(measurements, 2);

    std::ignore = getOrBuildRowFormatCache(cache, proc, 2, 7).commandWidth.get(measure); // new font
    EXPECT_EQ(measurements, 3);

    // Every free-text width starts unmeasured in a fresh entry.
    const RowFormatCache fresh = buildRowFormatCache(proc);
    for (const auto* width : {&fresh.pidWidth,
                              &fresh.userWidth,
                              &fresh.statusWidth,
                              &fresh.nameWidth,
                              &fresh.commandWidth,
                              &fresh.gpuEnginesWidth,
                              &fresh.gpuDevicesWidth,
                              &fresh.publisherWidth,
                              &fresh.processTypeWidth})
    {
        EXPECT_FLOAT_EQ(width->width, ProcessRowFormat::LazyTextWidth::UNMEASURED_WIDTH);
    }
}

// ========== Decimal-aligned cells (#1201) ==========

TEST(ProcessRowFormatTest, ByteCellsMarkWhereTheUnitStarts)
{
    const AlignedCellText cell = alignedBytesCell(512.0 * 1024.0 * 1024.0, UI::Format::BYTE_UNIT_MB);
    EXPECT_EQ(cell.text, "512.0 MiB");
    ASSERT_TRUE(cell.hasUnit());
    EXPECT_EQ(cell.number(), "512.0");
    EXPECT_EQ(cell.unit(), " MiB");
    EXPECT_FLOAT_EQ(cell.numberWidth, AlignedCellText::UNMEASURED_WIDTH);

    const AlignedCellText rate = alignedBytesPerSecCell(1536.0, UI::Format::BYTE_UNIT_KB);
    EXPECT_EQ(rate.number(), "1.5");
    EXPECT_EQ(rate.unit(), " KiB/s");
}

TEST(ProcessRowFormatTest, PowerCellsSplitNumberFromUnit)
{
    const AlignedCellText watts = alignedPowerCell(1.25);
    EXPECT_EQ(watts.number(), "1.3"); // Halves round away from zero, as everywhere else
    EXPECT_EQ(watts.unit(), " W");
    const AlignedCellText milliwatts = alignedPowerCell(0.35);
    EXPECT_EQ(milliwatts.number(), "350.0");
    EXPECT_EQ(milliwatts.unit(), " mW");
}

TEST(ProcessRowFormatTest, CellsWithoutAUnitAreNotUnitAligned)
{
    const AlignedCellText dash = makeAlignedCellText("-");
    EXPECT_FALSE(dash.hasUnit());
    EXPECT_EQ(dash.number(), "-");
    EXPECT_TRUE(dash.unit().empty());

    // The row builder keeps the split for real values and leaves "-" / "N/A" plain.
    ProcessSnapshot snap;
    snap.memoryBytes = 3ULL * 1024 * 1024;
    snap.ioAvailable = false;
    const RowFormatCache fmt = buildRowFormatCache(snap);
    EXPECT_TRUE(fmt.resident.hasUnit());
    EXPECT_EQ(fmt.resident.unit(), " MiB");
    EXPECT_FALSE(fmt.ioRead.hasUnit());
    EXPECT_EQ(fmt.ioRead.text, UNAVAILABLE_CELL_TEXT);
}

} // namespace
} // namespace App
