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

    std::string text;
    mutable float width = UNMEASURED_WIDTH;
};

/// Wraps `text` for a RowFormatCache population site, deferring width measurement to the first
/// time ProcessesPanel's renderRightAlignedText() actually draws this cell (see AlignedCellText's
/// doc comment).
[[nodiscard]] inline AlignedCellText makeAlignedCellText(std::string text)
{
    return AlignedCellText{.text = std::move(text)};
}

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

[[nodiscard]] inline std::string formatAlignedBytesString(double bytes, UI::Format::ByteUnit unit)
{
    const auto parts = UI::Format::splitBytesForAlignmentFast(bytes, unit);
    const auto wholePart = parts.wholePart();
    std::string out;
    out.reserve(wholePart.size() + parts.unitPart.size() + 1);
    out.append(wholePart.data(), wholePart.size());
    out.push_back(parts.decimalDigit);
    out.append(parts.unitPart.data(), parts.unitPart.size());
    return out;
}

[[nodiscard]] inline std::string formatAlignedBytesPerSecString(double bytesPerSec, UI::Format::ByteUnit unit)
{
    const auto parts = UI::Format::splitBytesPerSecForAlignmentFast(bytesPerSec, unit);
    const auto wholePart = parts.wholePart();
    std::string out;
    out.reserve(wholePart.size() + parts.unitPart.size() + 1);
    out.append(wholePart.data(), wholePart.size());
    out.push_back(parts.decimalDigit);
    out.append(parts.unitPart.data(), parts.unitPart.size());
    return out;
}

[[nodiscard]] inline std::string formatAlignedPowerString(double watts)
{
    const auto parts = UI::Format::splitPowerForAlignment(watts);
    std::string out;
    out.reserve(parts.wholePart.size() + parts.decimalPart.size() + parts.unitPart.size());
    out.append(parts.wholePart);
    out.append(parts.decimalPart);
    out.append(parts.unitPart);
    return out;
}

/// Cache of pre-formatted strings for one process row, keyed externally by uniqueKey (see
/// ProcessesPanel::m_RowFormatCache). Built lazily, on demand, the first time a row is actually
/// rendered after its snapshot generation changes -- not eagerly for every process in the
/// snapshot -- so cost scales with visible rows, not total process count (perf-plan #843).
/// `generation`/`fontPtr` record what this entry was built from, so ProcessesPanel can detect
/// staleness (a new snapshot version, or a font/size/DPI change) without a separate map lookup.
struct RowFormatCache
{
    std::uint64_t generation = 0;  // Snapshot version this entry was built from.
    const void* fontPtr = nullptr; // ImFont* used to build this entry; opaque here to avoid an ImGui dependency.

    AlignedCellText ppid;       // formatId(parentPid)          — immutable
    AlignedCellText startTime;  // formatEpochDateTimeShort      — immutable
    AlignedCellText cpuTime;    // formatCpuTimeCompact          — changes at 1Hz
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
    AlignedCellText affinity;   // formatCpuAffinityMask         — rarely changes
    AlignedCellText gdiObjects; // formatIntLocalized(*gdiObjectCount) or "-"
};

/// Formats every RowFormatCache field for one process snapshot. Pure (no ImGui calls, no shared
/// state) so it can run on demand from renderProcessRow() for exactly the rows ImGuiListClipper
/// decides are visible, instead of eagerly for every process every time the snapshot version
/// changes. `generation`/`fontPtr` are stamped by the caller after construction (they're not
/// derivable from `proc` alone).
[[nodiscard]] inline RowFormatCache buildRowFormatCache(const Domain::ProcessSnapshot& proc)
{
    RowFormatCache fmt;
    fmt.ppid = makeAlignedCellText(UI::Format::formatId(proc.parentPid));
    fmt.startTime = makeAlignedCellText(UI::Format::formatEpochDateTimeShort(proc.startTimeEpoch));
    fmt.cpuTime = makeAlignedCellText(UI::Format::formatCpuTimeCompact(proc.cpuTimeSeconds));
    fmt.cpuPercent = makeAlignedCellText(formatAlignedPercentString(proc.cpuPercent));
    fmt.memPercent = makeAlignedCellText(formatAlignedPercentString(proc.memoryPercent));
    fmt.virtualMem = makeAlignedCellText(
        formatAlignedBytesString(static_cast<double>(proc.virtualBytes), UI::Format::unitForTotalBytes(proc.virtualBytes)));
    fmt.resident = makeAlignedCellText(
        formatAlignedBytesString(static_cast<double>(proc.memoryBytes), UI::Format::unitForTotalBytes(proc.memoryBytes)));
    fmt.peakRss = makeAlignedCellText(
        formatAlignedBytesString(static_cast<double>(proc.peakMemoryBytes), UI::Format::unitForTotalBytes(proc.peakMemoryBytes)));
    fmt.shared = makeAlignedCellText(
        formatAlignedBytesString(static_cast<double>(proc.sharedBytes), UI::Format::unitForTotalBytes(proc.sharedBytes)));
    fmt.ioRead = makeAlignedCellText(
        (proc.ioReadBytesPerSec > 0.0)
            ? formatAlignedBytesPerSecString(proc.ioReadBytesPerSec, UI::Format::unitForBytesPerSecond(proc.ioReadBytesPerSec))
            : "-");
    fmt.ioWrite = makeAlignedCellText(
        (proc.ioWriteBytesPerSec > 0.0)
            ? formatAlignedBytesPerSecString(proc.ioWriteBytesPerSec, UI::Format::unitForBytesPerSecond(proc.ioWriteBytesPerSec))
            : "-");
    fmt.netSent = makeAlignedCellText(
        (proc.netSentBytesPerSec > 0.0)
            ? formatAlignedBytesPerSecString(proc.netSentBytesPerSec, UI::Format::unitForBytesPerSecond(proc.netSentBytesPerSec))
            : "-");
    fmt.netRecv = makeAlignedCellText(
        (proc.netReceivedBytesPerSec > 0.0)
            ? formatAlignedBytesPerSecString(proc.netReceivedBytesPerSec, UI::Format::unitForBytesPerSecond(proc.netReceivedBytesPerSec))
            : "-");
    fmt.power = makeAlignedCellText(formatAlignedPowerString(proc.powerWatts));
    fmt.gpuPercent = makeAlignedCellText((proc.gpuUtilPercent > 0.0) ? formatAlignedPercentString(proc.gpuUtilPercent) : "-");
    fmt.gpuMemory =
        makeAlignedCellText((proc.gpuMemoryBytes > 0) ? formatAlignedBytesString(static_cast<double>(proc.gpuMemoryBytes),
                                                                                 UI::Format::unitForTotalBytes(proc.gpuMemoryBytes))
                                                      : "-");
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
    fmt.handles = makeAlignedCellText(UI::Format::formatOrDash(proc.handleCount, [](auto v) { return UI::Format::formatIntLocalized(v); }));
    fmt.pageFaults =
        makeAlignedCellText(UI::Format::formatOrDash(proc.pageFaults, [](auto v) { return UI::Format::formatIntLocalized(v); }));
    fmt.affinity = makeAlignedCellText(UI::Format::formatCpuAffinityMask(proc.cpuAffinityMask));
    fmt.gdiObjects = makeAlignedCellText(proc.gdiObjectCount.has_value() ? UI::Format::formatIntLocalized(*proc.gdiObjectCount) : "-");
    return fmt;
}

} // namespace App::ProcessRowFormat
