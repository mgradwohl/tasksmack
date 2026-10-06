#pragma once

// Pure per-row formatting logic extracted from ProcessesPanel (perf-plan #843 phase 3b), so it
// can be unit-tested directly instead of only indirectly through a live ImGui context. See
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern (also used by
// ProcessTreeFlatten.h, AdaptiveIntervalUtils.h, TitleBarGeometry.h). buildRowFormatCache() makes
// no ImGui calls at all -- it only formats strings from a ProcessSnapshot -- so there is no
// live-context blocker to extracting and testing it directly.

#include "Domain/ProcessSnapshot.h"
#include "UI/Format.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace App::ProcessRowFormat
{

/// A pre-formatted right-aligned cell's text plus its CalcTextSize width, measured lazily (on
/// first render, not at cache population time) and cached from then on (perf-plan #843 phase 1).
/// Populating widths eagerly for every process at cache-rebuild time -- before ImGuiListClipper
/// gets a chance to restrict work to visible rows -- would concentrate thousands of CalcTextSize
/// calls into a single snapshot-update frame on the app's "thousands of processes" scenario,
/// working directly against the frame-budget goal this cache exists to serve. `width` is
/// `mutable` so ProcessesPanel's renderRightAlignedText() can fill it in through a `const
/// AlignedCellText&` the first time this specific cell is actually drawn; every later frame
/// (until the next cache rebuild resets it) reuses the cached value. Bundling text+width in one
/// type still means a caller can't use one without the other being kept in sync.
struct AlignedCellText
{
    /// Sentinel meaning "not measured yet". Real widths are never negative.
    static constexpr float UNMEASURED_WIDTH = -1.0F;
    /// unitStart for a cell with no unit to align ("-", "N/A", a count).
    static constexpr std::size_t NO_UNIT = std::string::npos;

    std::string text;
    mutable float width = UNMEASURED_WIDTH;

    /// Where the unit starts in `text`, its leading space included: 5 in "512.0 MiB". A column of
    /// mixed units ("512.0 B", "1.5 KiB", "3.2 MiB") draws the number right-aligned against a
    /// fixed-width unit slot and the unit in the slot, so the decimal points line up (#1201).
    std::size_t unitStart = NO_UNIT;
    /// Width of text before unitStart, measured lazily like `width`.
    mutable float numberWidth = UNMEASURED_WIDTH;

    [[nodiscard]] bool hasUnit() const noexcept
    {
        return unitStart < text.size();
    }
    [[nodiscard]] std::string_view number() const noexcept
    {
        return {text.data(), hasUnit() ? unitStart : text.size()};
    }
    [[nodiscard]] std::string_view unit() const noexcept
    {
        return hasUnit() ? std::string_view{text.data() + unitStart, text.size() - unitStart} : std::string_view{};
    }
};

/// The CalcTextSize width of a free-text cell whose text lives in the ProcessSnapshot rather than in
/// RowFormatCache -- a name, a user, a command line -- measured the first time the cell is drawn and
/// reused until the entry is rebuilt (#1141). That rebuild happens on a new snapshot generation, which
/// is the only way the text can change, and on a font/size/DPI change (RowFormatCache::fontId), which
/// is the only other way the width can, so no further invalidation is needed. Measuring these every
/// frame cost a glyph lookup per character per visible cell, and command lines run to thousands of
/// characters. `mutable` for the same reason as AlignedCellText::width.
struct LazyTextWidth
{
    /// Sentinel meaning "not measured yet". Real widths are never negative.
    static constexpr float UNMEASURED_WIDTH = -1.0F;

    mutable float width = UNMEASURED_WIDTH;

    /// The cached width, or `measure()`'s result -- remembered -- the first time.
    // measure is called at most once, so it is used as an lvalue rather than forwarded.
    template<typename Measure> [[nodiscard]] float get(Measure&& measure) const // NOLINT(cppcoreguidelines-missing-std-forward)
    {
        if (width < 0.0F)
        {
            width = measure();
        }
        return width;
    }
};

/// Wraps `text` for a RowFormatCache population site, deferring width measurement to the first
/// time ProcessesPanel's renderRightAlignedText() actually draws this cell (see AlignedCellText's
/// doc comment).
[[nodiscard]] inline AlignedCellText makeAlignedCellText(std::string text)
{
    return AlignedCellText{.text = std::move(text)};
}

/// A cell whose value the probe could not read for this process -- for lack of rights, e.g. another
/// user's process without root (#1110) -- as distinct from "-", a value that is 0 or not applicable.
inline constexpr std::string_view UNAVAILABLE_CELL_TEXT = "N/A";

/// Which optional fields the process probe fills; a field it does not is shown as "-".
struct RowFormatOptions
{
    bool hasPowerUsage = true;
    bool hasSharedMemory = true;
};

[[nodiscard]] inline std::string formatAlignedPercentString(double percent)
{
    const auto parts = UI::Format::splitPercentForAlignment(percent);

    std::string out;
    out.reserve(parts.wholePart.size() + 2);
    out.append(parts.wholePart.data(), parts.wholePart.size());
    out.push_back(parts.decimalDigit);
    out.append(UI::Format::AlignedPercentParts::unitPart.data(), UI::Format::AlignedPercentParts::unitPart.size());
    return out;
}

/// A byte or byte-rate cell from its split parts, with unitStart marking where the unit begins.
[[nodiscard]] inline AlignedCellText makeAlignedBytesCell(const UI::Format::AlignedBytesParts& parts)
{
    const auto wholePart = parts.wholePart();
    AlignedCellText cell;
    cell.text.reserve(wholePart.size() + parts.unitPart.size() + 1);
    cell.text.append(wholePart.data(), wholePart.size());
    cell.text.push_back(parts.decimalDigit);
    cell.unitStart = cell.text.size();
    cell.text.append(parts.unitPart.data(), parts.unitPart.size());
    return cell;
}

/// "512.0 MiB", decimal-aligned on its unit (see AlignedCellText::unitStart).
[[nodiscard]] inline AlignedCellText alignedBytesCell(double bytes, UI::Format::ByteUnit unit)
{
    return makeAlignedBytesCell(UI::Format::splitBytesForAlignmentFast(bytes, unit));
}

/// "1.5 MiB/s", decimal-aligned on its unit.
[[nodiscard]] inline AlignedCellText alignedBytesPerSecCell(double bytesPerSec, UI::Format::ByteUnit unit)
{
    return makeAlignedBytesCell(UI::Format::splitBytesPerSecForAlignmentFast(bytesPerSec, unit));
}

/// "1.2 W" or "350.0 mW", decimal-aligned on its unit: AlignedNumericParts' whole and decimal parts
/// are the number, its unitPart the unit.
[[nodiscard]] inline AlignedCellText alignedPowerCell(double watts)
{
    const auto parts = UI::Format::splitPowerForAlignment(watts);
    AlignedCellText cell;
    cell.text.reserve(parts.wholePart.size() + parts.decimalPart.size() + parts.unitPart.size());
    cell.text.append(parts.wholePart);
    cell.text.append(parts.decimalPart);
    cell.unitStart = cell.text.size();
    cell.text.append(parts.unitPart);
    return cell;
}

[[nodiscard]] inline std::string formatAlignedBytesString(double bytes, UI::Format::ByteUnit unit)
{
    return alignedBytesCell(bytes, unit).text;
}

[[nodiscard]] inline std::string formatAlignedBytesPerSecString(double bytesPerSec, UI::Format::ByteUnit unit)
{
    return alignedBytesPerSecCell(bytesPerSec, unit).text;
}

[[nodiscard]] inline std::string formatAlignedPowerString(double watts)
{
    return alignedPowerCell(watts).text;
}

/// Cache of pre-formatted strings for one process row, keyed externally by uniqueKey (see
/// ProcessesPanel::m_RowFormatCache). Built lazily, on demand, the first time a row is actually
/// rendered after its snapshot generation changes -- not eagerly for every process in the
/// snapshot -- so cost scales with visible rows, not total process count (perf-plan #843).
/// `generation`/`fontId` record what this entry was built from, so ProcessesPanel can detect
/// staleness (a new snapshot version, or a font/size/DPI change) without a separate map lookup.
struct RowFormatCache
{
    std::uint64_t generation = 0; // Snapshot version this entry was built from.
    // Identity of the ImFont this entry was built under. Deliberately a std::uintptr_t rather
    // than an ImFont*/const void*: it is only ever compared for equality, never dereferenced, and
    // storing it as an integer keeps a caller-supplied address from escaping into this long-lived
    // map (CodeQL cpp/stack-address-escape, see #904). Also keeps this header ImGui-free.
    std::uintptr_t fontId = 0;

    AlignedCellText ppid;       // formatId(parentPid)          — immutable
    AlignedCellText startTime;  // formatEpochDateTimeShort      — immutable
    AlignedCellText cpuTime;    // formatDuration                — changes at 1Hz
    AlignedCellText cpuPercent; // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText memPercent; // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText virtualMem; // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText resident;   // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText peakRss;    // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText shared;     // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText ioRead;     // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText ioWrite;    // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText netSent;    // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText netRecv;    // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText power;      // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText gpuPercent; // pre-formatted to avoid per-frame decimal alignment work
    AlignedCellText gpuMemory;  // pre-formatted to avoid per-frame decimal alignment work
    std::string gpuEngines;     // comma-joined engine list; avoids per-frame string joins; left-aligned, no width needed
    AlignedCellText threads;    // formatOrDash/formatIntLocalized(threadCount)
    AlignedCellText handles;    // formatOrDash/formatIntLocalized(handleCount)
    AlignedCellText pageFaults; // formatOrDash/formatIntLocalized(pageFaults)
    AlignedCellText affinity;   // formatCpuAffinity             — rarely changes
    AlignedCellText gdiObjects; // formatIntLocalized(*gdiObjectCount) or "-"

    // Widths of the cells drawn straight from the snapshot's own text (#1141); see LazyTextWidth.
    LazyTextWidth pidWidth;
    LazyTextWidth userWidth;
    LazyTextWidth statusWidth;
    LazyTextWidth nameWidth;
    LazyTextWidth commandWidth;
    LazyTextWidth gpuEnginesWidth; // of gpuEngines above
    LazyTextWidth gpuDevicesWidth;
    LazyTextWidth publisherWidth;
    LazyTextWidth processTypeWidth;
};

/// Formats every RowFormatCache field for one process snapshot. Pure (no ImGui calls, no shared
/// state) so it can run on demand from renderProcessRow() for exactly the rows ImGuiListClipper
/// decides are visible, instead of eagerly for every process every time the snapshot version
/// changes. `generation`/`fontId` are stamped by the caller after construction (they're not
/// derivable from `proc` alone).
///
/// `options` carries the process probe's capabilities for fields a platform may not fill: there the
/// cell reads "-", as the GPU cells do for no data, instead of a column of zeros that reads like a
/// measurement (#1028, #1035). They are fixed for the probe's lifetime, so they need no stamp.
[[nodiscard]] inline RowFormatCache buildRowFormatCache(const Domain::ProcessSnapshot& proc, const RowFormatOptions& options = {})
{
    RowFormatCache fmt;
    fmt.ppid = makeAlignedCellText(UI::Format::formatId(proc.parentPid));
    fmt.startTime = makeAlignedCellText(UI::Format::formatEpochDateTimeShort(proc.startTimeEpoch));
    fmt.cpuTime = makeAlignedCellText(UI::Format::formatDuration(proc.cpuTimeSeconds));
    fmt.cpuPercent = makeAlignedCellText(formatAlignedPercentString(proc.cpuPercent));
    fmt.memPercent = makeAlignedCellText(formatAlignedPercentString(proc.memoryPercent));
    const auto bytesCell = [](std::uint64_t bytes)
    {
        return alignedBytesCell(static_cast<double>(bytes), UI::Format::unitForTotalBytes(bytes));
    };
    fmt.virtualMem = bytesCell(proc.virtualBytes);
    fmt.resident = bytesCell(proc.memoryBytes);
    fmt.peakRss = bytesCell(proc.peakMemoryBytes);
    fmt.shared = options.hasSharedMemory ? bytesCell(proc.sharedBytes) : makeAlignedCellText("-");
    // A rate that is 0 reads "-"; one the probe could not read reads "N/A" (#1110) -- without root, every
    // other user's process used to show the same "-" as an idle one.
    const auto rateCell = [](bool available, double bytesPerSec) -> AlignedCellText
    {
        if (!available)
        {
            return makeAlignedCellText(std::string(UNAVAILABLE_CELL_TEXT));
        }
        return (bytesPerSec > 0.0) ? alignedBytesPerSecCell(bytesPerSec, UI::Format::unitForBytesPerSecond(bytesPerSec))
                                   : makeAlignedCellText("-");
    };
    fmt.ioRead = rateCell(proc.ioAvailable, proc.ioReadBytesPerSec);
    fmt.ioWrite = rateCell(proc.ioAvailable, proc.ioWriteBytesPerSec);
    fmt.netSent = rateCell(proc.networkAvailable, proc.netSentBytesPerSec);
    fmt.netRecv = rateCell(proc.networkAvailable, proc.netReceivedBytesPerSec);
    fmt.power = options.hasPowerUsage ? alignedPowerCell(proc.powerWatts) : makeAlignedCellText("-");
    fmt.gpuPercent = makeAlignedCellText((proc.gpuUtilPercent > 0.0) ? formatAlignedPercentString(proc.gpuUtilPercent) : "-");
    fmt.gpuMemory = (proc.gpuMemoryBytes > 0) ? bytesCell(proc.gpuMemoryBytes) : makeAlignedCellText("-");
    if (proc.gpuEngines.empty())
    {
        fmt.gpuEngines = "-";
    }
    else
    {
        for (size_t i = 0; i < proc.gpuEngines.size(); ++i)
        {
            if (i > 0)
            {
                fmt.gpuEngines += ", ";
            }
            fmt.gpuEngines += proc.gpuEngines[i];
        }
    }
    fmt.threads = makeAlignedCellText(UI::Format::formatOrDash(proc.threadCount, [](auto v) { return UI::Format::formatIntLocalized(v); }));
    fmt.handles = makeAlignedCellText(
        proc.handleCountAvailable ? UI::Format::formatOrDash(proc.handleCount, [](auto v) { return UI::Format::formatIntLocalized(v); })
                                  : std::string(UNAVAILABLE_CELL_TEXT));
    fmt.pageFaults =
        makeAlignedCellText(UI::Format::formatOrDash(proc.pageFaults, [](auto v) { return UI::Format::formatIntLocalized(v); }));
    fmt.affinity = makeAlignedCellText(UI::Format::formatCpuAffinity(proc.cpuAffinity.words()));
    fmt.gdiObjects = makeAlignedCellText(proc.gdiObjectCount.has_value() ? UI::Format::formatIntLocalized(*proc.gdiObjectCount) : "-");
    return fmt;
}

/// Get-or-build one row's cache entry: reuses it as-is if it was already built for this exact
/// `generation`/`fontId`, otherwise (re)builds it from `proc` via buildRowFormatCache() and
/// stamps the new generation/fontId. ImGui-free (only touches the map and calls
/// buildRowFormatCache()) so the lazy-invalidation policy itself -- not just the formatting it
/// produces -- is directly unit-testable, separate from renderProcessRow()'s live ImGui context.
inline RowFormatCache& getOrBuildRowFormatCache(std::unordered_map<std::uint64_t, RowFormatCache>& cache,
                                                const Domain::ProcessSnapshot& proc,
                                                std::uint64_t generation,
                                                std::uintptr_t fontId,
                                                const RowFormatOptions& options = {})
{
    // try_emplace, not operator[]: a freshly default-constructed entry carries generation == 0 /
    // fontId == 0, which are themselves legal stamp values, so a stamp comparison alone
    // can't tell "never built" from "already built for generation 0 with no font" and would hand
    // back an empty, unformatted entry. `inserted` makes first access unambiguous, independent of
    // whatever the caller happens to seed its generation counter and font identity with.
    const auto [it, inserted] = cache.try_emplace(proc.uniqueKey);
    RowFormatCache& entry = it->second;
    if (inserted || entry.generation != generation || entry.fontId != fontId)
    {
        entry = buildRowFormatCache(proc, options);
        entry.generation = generation;
        entry.fontId = fontId;
    }
    return entry;
}

} // namespace App::ProcessRowFormat
