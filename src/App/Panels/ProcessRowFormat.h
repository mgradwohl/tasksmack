#pragma once

// Pure per-row formatting logic extracted from ProcessesPanel (perf-plan #843 phase 3b), so it
// can be unit-tested directly instead of only indirectly through a live ImGui context. See
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern (also used by
// ProcessTreeFlatten.h, AdaptiveIntervalUtils.h, TitleBarGeometry.h). buildRowFormatCache() makes
// no ImGui calls at all -- it only formats strings from a ProcessSnapshot -- so there is no
// live-context blocker to extracting and testing it directly.

#include "Domain/ProcessSnapshot.h"
#include "UI/Format.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace App::ProcessRowFormat
{

/// How a cell's value reads beside a measured one (#1210). A measured zero and a value TaskSmack does
/// not have are different facts, and used to share one "-".
enum class CellTone : std::uint8_t
{
    Value,       ///< A measured value
    Zero,        ///< A measured zero, drawn muted so it recedes without reading as "no data"
    Unavailable, ///< No value: UNAVAILABLE_CELL_TEXT, muted, with AlignedCellText::unavailableReason as its tooltip
};

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
    /// unitStart for a cell with no unit to align (UNAVAILABLE_CELL_TEXT, a count).
    static constexpr std::size_t NO_UNIT = std::string::npos;

    std::string text;
    mutable float width = UNMEASURED_WIDTH;

    /// Where the unit starts in `text`, its leading space included: 5 in "512.0 MiB". A column of
    /// mixed units ("512.0 B", "1.5 KiB", "3.2 MiB") draws the number right-aligned against a
    /// fixed-width unit slot and the unit in the slot, so the decimal points line up (#1201).
    std::size_t unitStart = NO_UNIT;
    /// Width of text before unitStart, measured lazily like `width`.
    mutable float numberWidth = UNMEASURED_WIDTH;

    CellTone tone = CellTone::Value;
    /// Why there is no value, for the cell's tooltip: a string literal, null unless tone is Unavailable.
    const char* unavailableReason = nullptr;

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

/// The one glyph the table shows for a value it does not have, whatever the reason (#1210): an em
/// dash, drawn muted. A measured zero reads as a (muted) zero instead, never as this.
inline constexpr std::string_view UNAVAILABLE_CELL_TEXT = "\xE2\x80\x94";

/// Tooltip of a cell the probe could not read for this one process -- for lack of rights, e.g.
/// another user's process without root (#1110).
inline constexpr const char* UNREADABLE_CELL_REASON =
    "Not available: TaskSmack could not read this for this process, usually for lack of privileges.";

/// Tooltip of a GPU cell whose generation's per-process GPU read failed, on a system that has the
/// data (#1210): a gap in one sample, not a lack of support.
inline constexpr const char* GPU_READ_FAILED_CELL_REASON = "Not available: reading per-process GPU data failed for this sample.";

/// Tooltip of a GPU cell of a process that started since per-process GPU data was last read by the
/// GPU sampler (#1210, #1417): its GPU figures have not been read yet.
inline constexpr const char* GPU_NOT_READ_YET_CELL_REASON = "Not available yet: this process started since GPU usage was last read.";

/// Tooltip of a cell in a column this system's process probe cannot fill at all (#1028, #1035).
inline constexpr const char* UNSUPPORTED_CELL_REASON = "Not available on this system.";

/// Which optional fields the process probe can fill (Platform::ProcessCapabilities). A column it
/// cannot reads UNAVAILABLE_CELL_TEXT on every row, rather than a column of zeros that reads like a
/// measurement (#1028, #1035).
struct RowFormatOptions
{
    bool hasPowerUsage = true;
    bool hasSharedMemory = true;
    bool hasIoCounters = true;
    bool hasNetworkCounters = true;
    bool hasThreadCount = true;
    bool hasHandleCount = true;
    bool hasPageFaults = true;
    bool hasCpuAffinity = true;
    bool hasGdiObjects = true;
    bool hasPerProcessGpu = true;            ///< Platform::GPUCapabilities::hasPerProcessMetrics, from the GPU probe (#1210)
    bool hasPerProcessGpuUtilization = true; ///< Platform::GPUCapabilities::hasPerProcessUtilization (#1210)
    bool gpuReadFailed = false;              ///< Supported, but this generation's per-process GPU read failed (#1210)
    bool hasStatus = true;
    bool hasPublisher = true;
    bool hasProcessType = true;
};

/// A cell with no value: UNAVAILABLE_CELL_TEXT, with `reason` (a string literal) as its tooltip.
[[nodiscard]] inline AlignedCellText unavailableCell(const char* reason)
{
    AlignedCellText cell = makeAlignedCellText(std::string(UNAVAILABLE_CELL_TEXT));
    cell.tone = CellTone::Unavailable;
    cell.unavailableReason = reason;
    return cell;
}

/// `cell`, marked as a measured zero when `isZero`.
[[nodiscard]] inline AlignedCellText withZeroTone(AlignedCellText cell, bool isZero)
{
    if (isZero)
    {
        cell.tone = CellTone::Zero;
    }
    return cell;
}

/// Whether a value shown with one decimal digit reads as zero ("0.0"). NaN and negatives do too.
[[nodiscard]] constexpr bool readsAsZeroAtOneDecimal(double value) noexcept
{
    constexpr double HALF_OF_LAST_DIGIT = 0.05;
    return !(value >= HALF_OF_LAST_DIGIT);
}

/// A count ("1,234"), marked as a measured zero when it is 0.
template<std::integral T> [[nodiscard]] inline AlignedCellText countCell(T value)
{
    return withZeroTone(makeAlignedCellText(UI::Format::formatIntLocalized(value)), value == T{0});
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
    std::string gpuEngines;     // comma-joined engine list, empty for none; avoids per-frame string joins
    AlignedCellText threads;    // countCell(threadCount), or unavailable
    AlignedCellText handles;    // countCell(handleCount), or unavailable
    AlignedCellText pageFaults; // countCell(pageFaults), or unavailable
    AlignedCellText affinity;   // formatCpuAffinity             — rarely changes
    AlignedCellText gdiObjects; // countCell(*gdiObjectCount), or unavailable

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

    // Whether this system can fill the free-text columns drawn straight from the snapshot (#1210):
    // where it cannot, the cell is the unavailable dash rather than blank. Copied from the
    // RowFormatOptions the entry was built with, so the cell renderers need no capabilities.
    bool statusSupported = true;
    bool publisherSupported = true;
    bool processTypeSupported = true;
    bool gpuSupported = true;   // GPU Engine and GPU Device; GPU % and GPU Memory carry their own tone
    bool gpuReadFailed = false; // Supported, but this generation's per-process GPU read failed (#1210)
    // Why this row's GPU figures are not readings although the system has them -- a failed read, or a
    // process the last GPU read did not see -- or null when they are (#1210).
    const char* gpuUnreadReason = nullptr;
};

/// Formats every RowFormatCache field for one process snapshot. Pure (no ImGui calls, no shared
/// state) so it can run on demand from renderProcessRow() for exactly the rows ImGuiListClipper
/// decides are visible, instead of eagerly for every process every time the snapshot version
/// changes. `generation`/`fontId` are stamped by the caller after construction (they're not
/// derivable from `proc` alone).
///
/// A value TaskSmack does not have -- in a column the probe cannot fill on this system (`options`,
/// #1028, #1035), or one it could not read for this process (#1110) -- is UNAVAILABLE_CELL_TEXT with
/// the reason as its tooltip. A measured zero keeps its number in the column's own format and is
/// marked CellTone::Zero, so the two never look alike (#1210). The capabilities are published with
/// the snapshot generation the entry is stamped with, so they need no stamp of their own.
[[nodiscard]] inline RowFormatCache buildRowFormatCache(const Domain::ProcessSnapshot& proc, const RowFormatOptions& options = {})
{
    RowFormatCache fmt;
    fmt.statusSupported = options.hasStatus;
    fmt.publisherSupported = options.hasPublisher;
    fmt.processTypeSupported = options.hasProcessType;
    fmt.gpuSupported = options.hasPerProcessGpu;
    fmt.gpuReadFailed = options.hasPerProcessGpu && options.gpuReadFailed;
    if (options.hasPerProcessGpu)
    {
        if (options.gpuReadFailed)
        {
            fmt.gpuUnreadReason = GPU_READ_FAILED_CELL_REASON;
        }
        else if (!proc.gpuFieldsRead)
        {
            fmt.gpuUnreadReason = GPU_NOT_READ_YET_CELL_REASON;
        }
    }
    fmt.ppid = makeAlignedCellText(UI::Format::formatId(proc.parentPid));
    fmt.startTime = (proc.startTimeEpoch != 0) ? makeAlignedCellText(UI::Format::formatEpochDateTimeShort(proc.startTimeEpoch))
                                               : unavailableCell(UNREADABLE_CELL_REASON);
    fmt.cpuTime = makeAlignedCellText(UI::Format::formatDuration(proc.cpuTimeSeconds));
    fmt.cpuPercent =
        withZeroTone(makeAlignedCellText(formatAlignedPercentString(proc.cpuPercent)), readsAsZeroAtOneDecimal(proc.cpuPercent));
    fmt.memPercent =
        withZeroTone(makeAlignedCellText(formatAlignedPercentString(proc.memoryPercent)), readsAsZeroAtOneDecimal(proc.memoryPercent));
    const auto bytesCell = [](std::uint64_t bytes)
    {
        return withZeroTone(alignedBytesCell(static_cast<double>(bytes), UI::Format::unitForTotalBytes(bytes)), bytes == 0);
    };
    fmt.virtualMem = bytesCell(proc.virtualBytes);
    fmt.resident = bytesCell(proc.memoryBytes);
    // Not gated on ProcessCapabilities::hasPeakRss: where the OS has no peak, the model tracks one.
    fmt.peakRss = bytesCell(proc.peakMemoryBytes);
    fmt.shared = options.hasSharedMemory ? bytesCell(proc.sharedBytes) : unavailableCell(UNSUPPORTED_CELL_REASON);
    // An idle rate is a measured "0.0 B/s"; one the probe could not read is unavailable (#1110) --
    // without root, every other user's process used to look as idle as a process that was.
    const auto rateCell = [](bool supported, bool available, double bytesPerSec) -> AlignedCellText
    {
        if (!supported)
        {
            return unavailableCell(UNSUPPORTED_CELL_REASON);
        }
        if (!available)
        {
            return unavailableCell(UNREADABLE_CELL_REASON);
        }
        const double rate = (bytesPerSec > 0.0) ? bytesPerSec : 0.0;
        return withZeroTone(alignedBytesPerSecCell(rate, UI::Format::unitForBytesPerSecond(rate)), readsAsZeroAtOneDecimal(rate));
    };
    fmt.ioRead = rateCell(options.hasIoCounters, proc.ioAvailable, proc.ioReadBytesPerSec);
    fmt.ioWrite = rateCell(options.hasIoCounters, proc.ioAvailable, proc.ioWriteBytesPerSec);
    fmt.netSent = rateCell(options.hasNetworkCounters, proc.networkAvailable, proc.netSentBytesPerSec);
    fmt.netRecv = rateCell(options.hasNetworkCounters, proc.networkAvailable, proc.netReceivedBytesPerSec);
    // Power is shown down to microwatts, so it reads as zero below a twentieth of one.
    constexpr double MICROWATTS_PER_WATT = 1.0e6;
    fmt.power = options.hasPowerUsage
                  ? withZeroTone(alignedPowerCell(proc.powerWatts), readsAsZeroAtOneDecimal(proc.powerWatts * MICROWATTS_PER_WATT))
                  : unavailableCell(UNSUPPORTED_CELL_REASON);
    // Where the GPU probe has no per-process metrics (DRM- or ROCm-only Linux), every process reads 0:
    // that is no measurement (#1210). Nor is GPU % where it has memory but not utilization (NVML).
    // A supported read that failed for this generation is a gap in one sample, not either of those.
    if (!(options.hasPerProcessGpu && options.hasPerProcessGpuUtilization))
    {
        fmt.gpuPercent = unavailableCell(UNSUPPORTED_CELL_REASON);
    }
    else if (fmt.gpuUnreadReason != nullptr)
    {
        fmt.gpuPercent = unavailableCell(fmt.gpuUnreadReason);
    }
    else
    {
        fmt.gpuPercent = withZeroTone(makeAlignedCellText(formatAlignedPercentString(proc.gpuUtilPercent)),
                                      readsAsZeroAtOneDecimal(proc.gpuUtilPercent));
    }
    if (!options.hasPerProcessGpu)
    {
        fmt.gpuMemory = unavailableCell(UNSUPPORTED_CELL_REASON);
    }
    else
    {
        fmt.gpuMemory = (fmt.gpuUnreadReason != nullptr) ? unavailableCell(fmt.gpuUnreadReason) : bytesCell(proc.gpuMemoryBytes);
    }
    // No engine in use is a fact rather than a gap, so it is left blank rather than marked unavailable.
    for (std::size_t i = 0; i < proc.gpuEngines.size(); ++i)
    {
        if (i > 0)
        {
            fmt.gpuEngines += ", ";
        }
        fmt.gpuEngines += proc.gpuEngines[i];
    }
    // A running process has at least one thread, so a count of 0 is one that was not read.
    if (!options.hasThreadCount)
    {
        fmt.threads = unavailableCell(UNSUPPORTED_CELL_REASON);
    }
    else
    {
        fmt.threads = (proc.threadCount > 0) ? countCell(proc.threadCount) : unavailableCell(UNREADABLE_CELL_REASON);
    }
    if (!options.hasHandleCount)
    {
        fmt.handles = unavailableCell(UNSUPPORTED_CELL_REASON);
    }
    else
    {
        fmt.handles =
            (proc.handleCountAvailable && proc.handleCount >= 0) ? countCell(proc.handleCount) : unavailableCell(UNREADABLE_CELL_REASON);
    }
    fmt.pageFaults = options.hasPageFaults ? countCell(proc.pageFaults) : unavailableCell(UNSUPPORTED_CELL_REASON);
    // An empty affinity is one that was not read (ProcessSnapshot::cpuAffinity): a process can always run somewhere.
    if (!options.hasCpuAffinity)
    {
        fmt.affinity = unavailableCell(UNSUPPORTED_CELL_REASON);
    }
    else
    {
        fmt.affinity = !proc.cpuAffinity.empty() ? makeAlignedCellText(UI::Format::formatCpuAffinity(proc.cpuAffinity.words()))
                                                 : unavailableCell(UNREADABLE_CELL_REASON);
    }
    // No GDI count means the process could not be opened; 0 is a background process that owns none.
    if (!options.hasGdiObjects)
    {
        fmt.gdiObjects = unavailableCell(UNSUPPORTED_CELL_REASON);
    }
    else
    {
        fmt.gdiObjects = proc.gdiObjectCount.has_value() ? countCell(*proc.gdiObjectCount) : unavailableCell(UNREADABLE_CELL_REASON);
    }
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
