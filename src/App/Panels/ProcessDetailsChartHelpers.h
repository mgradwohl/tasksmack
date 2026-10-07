#pragma once

// The pure pieces of Process Details' chart tabs (Overview, Network and I/O, GPU): the NowBar column
// counts, the value text, each chart's axis target and the tabs' empty-state decisions. Extracted
// with the charts themselves (ProcessDetailsCharts, #1179 slice 5) so they are unit-testable without
// a live ImGui context, following CONTRIBUTING.md's "extract the pure decision logic into a small
// header" pattern.

#include "Domain/ProcessSnapshot.h"
#include "ProcessDetailsHistory.h"
#include "ProcessDetailsPanel_GpuHelpers.h"
#include "ProcessDetailsPanel_HistoryHelpers.h"
#include "ProcessSmoothedUsage.h"
#include "UI/Format.h"
#include "UI/RateAxis.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>

namespace App::Detail
{

// The NowBar columns each Process Details tab's charts reserve: the most bars any chart on that tab
// has, so charts stacked on one tab are the same width and their time axes line up (#1206), without
// a tab of one- or two-bar charts keeping empty columns for another tab's widest chart.
//
// Overview (CPU, Memory, Power, Resources): Resources has four on Windows (with GDI Objects); with a
// column of its own it was narrower than the CPU and Memory charts above it, and its time axis shorter.
#ifdef _WIN32
inline constexpr std::size_t PROCESS_OVERVIEW_NOW_BAR_COLUMNS = 4;
#else
inline constexpr std::size_t PROCESS_OVERVIEW_NOW_BAR_COLUMNS = 3;
#endif
// Network and I/O: Read and Write, Sent and Received.
inline constexpr std::size_t PROCESS_NETWORK_IO_NOW_BAR_COLUMNS = 2;
// GPU: Utilization, and Memory, one bar each.
inline constexpr std::size_t PROCESS_GPU_NOW_BAR_COLUMNS = 1;

/// A count history sample as text, or N/A for a non-finite one (an unread value or a gap, #1110 /
/// #1098): std::llround of NaN is unspecified, so it must not reach formatIntLocalized().
[[nodiscard]] inline std::string formatCountOrNA(double value)
{
    return std::isfinite(value) ? UI::Format::formatIntLocalized(std::llround(value)) : std::string("N/A");
}

/// "412.0 MB (1.3% of RAM)": the bytes the Memory chart plots and the share of RAM the Processes
/// table shows, from @p percentPerByte (ProcessSmoothedUsage::memoryPercentPerByte). N/A for a gap
/// point (#1098). The share is held to [0, 100].
[[nodiscard]] inline std::string formatBytesWithRamShare(double bytes, double percentPerByte)
{
    if (!std::isfinite(bytes))
    {
        return "N/A"; // a gap point (#1098)
    }
    return std::format(
        "{} ({} of RAM)", UI::Format::formatBytes(bytes), UI::Format::percentOneDecimal(std::clamp(bytes * percentPerByte, 0.0, 100.0)));
}

/// A smoothed I/O or network rate as its NowBar shows it, in the unit that suits it, or N/A when the
/// counter could not be read (#1110), as its line shows a gap.
[[nodiscard]] inline std::string rateTextOrNA(bool available, double bytesPerSec)
{
    return available ? UI::Format::formatBytesPerSecWithUnit(bytesPerSec, UI::Format::unitForBytesPerSecond(bytesPerSec))
                     : std::string("N/A");
}

/// A smoothed count NowBar's text (threads, handles, GDI objects), or N/A while it has no reading.
[[nodiscard]] inline std::string countTextOrNA(bool available, double count)
{
    return available ? UI::Format::formatIntLocalized(std::llround(count)) : std::string("N/A");
}

/// @p names as one "a, b, c" line: the GPU tab's active engines.
[[nodiscard]] inline std::string joinWithCommas(std::span<const std::string> names)
{
    std::string joined;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (i > 0)
        {
            joined += ", ";
        }
        joined += names[i];
    }
    return joined;
}

/// Whether the GPU tab lists the Dedicated and Shared rows under a GPU Memory total. That total counts
/// what each GPU's "used" figure on the GPU tab counts (#1164); both kinds are listed whenever it
/// doesn't already show them: shared memory is mapped, or the dedicated bytes aren't what was counted
/// (a shared-segment GPU with no shared use yet).
[[nodiscard]] constexpr bool showsGpuMemoryKinds(std::uint64_t totalBytes, std::uint64_t dedicatedBytes, std::uint64_t sharedBytes) noexcept
{
    return sharedBytes > 0 || dedicatedBytes != totalBytes;
}

// Each chart's axis target: the largest value it draws in the window (UI::Widgets::maxOfSeriesSince(),
// #1145) and the smoothed values of its bars, which can still be easing down from a peak that has
// just left the window. Each bar is scaled to its series' axis, so a bar and its line show a value at
// the same height (#1003). An unread value (NaN, or a bar showing N/A) does not move an axis.

/// The CPU chart's: Total, User and System on one percent axis (#1195).
[[nodiscard]] inline double cpuAxisDataMax(std::span<const double> timeData,
                                           double xMin,
                                           std::span<const double> total,
                                           std::span<const double> user,
                                           std::span<const double> system,
                                           const ProcessSmoothedUsage& smoothed) noexcept
{
    return std::max({UI::Widgets::maxOfSeriesSince(timeData, xMin, total, user, system),
                     smoothed.cpuPercent,
                     smoothed.cpuUserPercent,
                     smoothed.cpuSystemPercent});
}

/// The Memory chart's left axis: Used and Shared bytes (@p shared empty where it is not drawn), with
/// their bars' smoothed values. The lifetime peak is left out: one far above today's usage would
/// flatten the lines; its value is in the value strip and the tooltip.
[[nodiscard]] inline double memoryAxisDataMax(std::span<const double> timeData,
                                              double xMin,
                                              std::span<const double> used,
                                              std::span<const double> shared,
                                              const ProcessSmoothedUsage& smoothed) noexcept
{
    return std::max({UI::Widgets::maxOfSeriesSince(timeData, xMin, used, shared), smoothed.residentBytes, smoothed.memorySharedBytes});
}

/// The Memory chart's right-hand Virtual axis (#992).
[[nodiscard]] inline double virtualAxisDataMax(std::span<const double> timeData,
                                               double xMin,
                                               std::span<const double> virtualBytes,
                                               const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, xMin, virtualBytes), {smoothed.virtualBytes});
}

/// The Resources chart's count axis: Threads and Handles/FDs (#1024). An unreadable handle count
/// (#1110) is left out.
[[nodiscard]] inline double countAxisDataMax(std::span<const double> timeData,
                                             double xMin,
                                             std::span<const double> threads,
                                             std::span<const double> handles,
                                             const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(
        UI::Widgets::maxOfSeriesSince(timeData, xMin, threads, handles),
        {smoothed.threadCount, UI::Widgets::currentIfAvailable(smoothed.handleCountAvailable, smoothed.handleCount)});
}

/// countAxisDataMax() with the GDI Objects series too (Windows, #1000). @p gdi can be shorter than
/// the others; it ends at the same newest sample. A GDI bar with no reading is left out.
[[nodiscard]] inline double countAxisDataMaxWithGdi(std::span<const double> timeData,
                                                    double xMin,
                                                    std::span<const double> threads,
                                                    std::span<const double> handles,
                                                    std::span<const double> gdi,
                                                    const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(
        std::max(UI::Widgets::maxOfSeriesSince(timeData, xMin, threads, handles), UI::Widgets::maxOfSeriesSince(timeData, xMin, gdi)),
        {smoothed.threadCount,
         UI::Widgets::currentIfAvailable(smoothed.handleCountAvailable, smoothed.handleCount),
         UI::Widgets::currentIfAvailable(smoothed.gdiInitialized, smoothed.gdiObjectCount)});
}

/// The Resources chart's right-hand Page Faults axis (#1024).
[[nodiscard]] inline double faultAxisDataMax(std::span<const double> timeData,
                                             double xMin,
                                             std::span<const double> faults,
                                             const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, xMin, faults), {smoothed.pageFaultsPerSec});
}

/// The I/O chart's: Read and Write, without the bars while the counters are unread (#1110).
[[nodiscard]] inline double ioAxisDataMax(std::span<const double> timeData,
                                          double xMin,
                                          std::span<const double> read,
                                          std::span<const double> write,
                                          const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, xMin, read, write),
                                          {UI::Widgets::currentIfAvailable(smoothed.ioAvailable, smoothed.ioReadBytesPerSec),
                                           UI::Widgets::currentIfAvailable(smoothed.ioAvailable, smoothed.ioWriteBytesPerSec)});
}

/// The Network chart's: Sent and Received, without the bars while they are unattributed (#1110).
[[nodiscard]] inline double networkAxisDataMax(std::span<const double> timeData,
                                               double xMin,
                                               std::span<const double> sent,
                                               std::span<const double> received,
                                               const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, xMin, sent, received),
                                          {UI::Widgets::currentIfAvailable(smoothed.networkAvailable, smoothed.netSentBytesPerSec),
                                           UI::Widgets::currentIfAvailable(smoothed.networkAvailable, smoothed.netRecvBytesPerSec)});
}

/// The Power chart's.
[[nodiscard]] inline double powerAxisDataMax(std::span<const double> timeData,
                                             double xMin,
                                             std::span<const double> power,
                                             const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, xMin, power), {smoothed.powerWatts});
}

/// The GPU Memory chart's, without the bar while the GPU probe gave no reading (#1210).
[[nodiscard]] inline double gpuMemoryAxisDataMax(std::span<const double> timeData,
                                                 double xMin,
                                                 std::span<const double> gpuMemory,
                                                 const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(timeData, xMin, gpuMemory),
                                          {UI::Widgets::currentIfAvailable(smoothed.gpuMemoryAvailable, smoothed.gpuMemoryBytes)});
}

/// What the GPU tab shows for @p proc and its retained @p history (gpuTabContent()). @p gpuSupported
/// is whether the GPU probe supports per-process data at all (SampleRateReadings::gpuSupported): a
/// failed read is not "not available on this system" (#1210).
[[nodiscard]] inline GpuTabContent
gpuTabContentFor(const Domain::ProcessSnapshot& proc, const ProcessDetailsHistory& history, bool gpuSupported)
{
    const std::span<const double> util = history.series(ProcessSeries::GpuUtil);
    const std::span<const double> memory = history.series(ProcessSeries::GpuMemory);
    return gpuTabContent(gpuSupported,
                         hasGpuUsageToShow(proc.gpuMemoryBytes, proc.gpuUtilPercent, !proc.gpuDevices.empty(), util, memory),
                         hasAnyReading(util) || hasAnyReading(memory));
}

/// Whether the Network and I/O tab has anything to chart, rather than its empty state (#1210).
/// Readings only: every sample adds a point, a gap where there was no reading, so a history that is
/// merely non-empty is not data.
[[nodiscard]] inline bool hasNetworkTabData(const ProcessDetailsHistory& history) noexcept
{
    return hasNetworkOrIoReadings(history.series(ProcessSeries::IoRead),
                                  history.series(ProcessSeries::IoWrite),
                                  history.series(ProcessSeries::NetSent),
                                  history.series(ProcessSeries::NetReceived));
}

} // namespace App::Detail
