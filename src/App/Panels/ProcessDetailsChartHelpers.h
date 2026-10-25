#pragma once

// The pure pieces of Process Details' chart tabs (Overview, Network and I/O, GPU, NPU): the NowBar column
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
// NPU (#1869): Utilization's one bar, and Memory's Dedicated and Shared.
inline constexpr std::size_t PROCESS_NPU_NOW_BAR_COLUMNS = 2;

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

/// The notice the Network chart shows over its plot when it has nothing to draw (#1918).
inline constexpr const char* NETWORK_NOT_AVAILABLE_NOTICE = "Network traffic not available for this process";
/// The I/O chart's.
inline constexpr const char* IO_NOT_AVAILABLE_NOTICE = "I/O not available for this process";

/// What a two-rate chart (Network, I/O) shows over its plot: @p notice while its rates are not read for
/// the process (@p available false) and its history holds no reading either, and null otherwise (#1918).
/// Without it the chart drew an empty 0 to 1 KiB/s axis, as a measured 0 B/s does. Once the window
/// holds readings, their lines and the gap after them say it.
[[nodiscard]] inline const char*
rateChartNotice(bool available, std::span<const double> first, std::span<const double> second, const char* notice) noexcept
{
    return (!available && !hasAnyReading(first) && !hasAnyReading(second)) ? notice : nullptr;
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
//
// Each comes in two forms: from the window's series maximum, which the charts keep between frames
// (UI::Widgets::windowSeriesMax(), #843), and from the series themselves, the same over
// maxOfSeriesSince().

/// The CPU chart's: Total, User and System on one percent axis (#1195). @p seriesMax is their maximum
/// in the window.
[[nodiscard]] inline double cpuAxisDataMax(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return std::max({seriesMax, smoothed.cpuPercent, smoothed.cpuUserPercent, smoothed.cpuSystemPercent});
}

[[nodiscard]] inline double cpuAxisDataMax(std::span<const double> timeData,
                                           double xMin,
                                           std::span<const double> total,
                                           std::span<const double> user,
                                           std::span<const double> system,
                                           const ProcessSmoothedUsage& smoothed) noexcept
{
    return cpuAxisDataMax(UI::Widgets::maxOfSeriesSince(timeData, xMin, total, user, system), smoothed);
}

/// The Memory chart's left axis: Used and Shared bytes (@p shared empty where it is not drawn), with
/// their bars' smoothed values, and Private and Swap where they are drawn (#1885; empty otherwise). The lifetime peak is left out: one far
/// above today's usage would flatten the lines; its value is in the value strip and the tooltip.
/// @p seriesMax is the drawn series' maximum in the window.
[[nodiscard]] inline double memoryAxisDataMax(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return std::max({seriesMax, smoothed.residentBytes, smoothed.memorySharedBytes});
}

[[nodiscard]] inline double memoryAxisDataMax(std::span<const double> timeData,
                                              double xMin,
                                              std::span<const double> used,
                                              std::span<const double> shared,
                                              const ProcessSmoothedUsage& smoothed,
                                              std::span<const double> privateBytes = {},
                                              std::span<const double> swapBytes = {}) noexcept
{
    return memoryAxisDataMax(UI::Widgets::maxOfSeriesSince(timeData, xMin, used, shared, privateBytes, swapBytes), smoothed);
}

/// The Memory chart's right-hand Virtual axis (#992).
[[nodiscard]] inline double virtualAxisDataMax(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(seriesMax, {smoothed.virtualBytes});
}

[[nodiscard]] inline double virtualAxisDataMax(std::span<const double> timeData,
                                               double xMin,
                                               std::span<const double> virtualBytes,
                                               const ProcessSmoothedUsage& smoothed) noexcept
{
    return virtualAxisDataMax(UI::Widgets::maxOfSeriesSince(timeData, xMin, virtualBytes), smoothed);
}

/// The Resources chart's left axis: Threads alone (#1916). Handles/FDs have their own right-hand
/// axis (handleAxisDataMax()): a process holds tens of times more of them than threads, so on one
/// shared axis the Threads line lay flat on the baseline. A Threads bar with no reading is left out (#1935).
[[nodiscard]] inline double threadAxisDataMax(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(seriesMax,
                                          {UI::Widgets::currentIfAvailable(smoothed.threadCountAvailable, smoothed.threadCount)});
}

[[nodiscard]] inline double threadAxisDataMax(std::span<const double> timeData,
                                              double xMin,
                                              std::span<const double> threads,
                                              const ProcessSmoothedUsage& smoothed) noexcept
{
    return threadAxisDataMax(UI::Widgets::maxOfSeriesSince(timeData, xMin, threads), smoothed);
}

/// threadAxisDataMax() with the GDI Objects series too (Windows, #1000), which shares the Threads
/// axis: a process's GDI objects run to the same order of magnitude as its threads, far closer than
/// its handles do (#1916). @p gdi can be shorter than the others; it ends at the same newest sample.
/// A GDI bar with no reading is left out. @p seriesMax is Threads' and GDI Objects' maximum in the
/// window.
[[nodiscard]] inline double threadAxisDataMaxWithGdi(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(seriesMax,
                                          {UI::Widgets::currentIfAvailable(smoothed.threadCountAvailable, smoothed.threadCount),
                                           UI::Widgets::currentIfAvailable(smoothed.gdiInitialized, smoothed.gdiObjectCount)});
}

[[nodiscard]] inline double threadAxisDataMaxWithGdi(std::span<const double> timeData,
                                                     double xMin,
                                                     std::span<const double> threads,
                                                     std::span<const double> gdi,
                                                     const ProcessSmoothedUsage& smoothed) noexcept
{
    return threadAxisDataMaxWithGdi(UI::Widgets::maxOfSeriesSince(timeData, xMin, threads, gdi), smoothed);
}

/// The Resources chart's Handles/FDs axis, the outer right-hand one (#1916). An unreadable handle
/// count (#1110) is left out.
[[nodiscard]] inline double handleAxisDataMax(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(seriesMax,
                                          {UI::Widgets::currentIfAvailable(smoothed.handleCountAvailable, smoothed.handleCount)});
}

[[nodiscard]] inline double handleAxisDataMax(std::span<const double> timeData,
                                              double xMin,
                                              std::span<const double> handles,
                                              const ProcessSmoothedUsage& smoothed) noexcept
{
    return handleAxisDataMax(UI::Widgets::maxOfSeriesSince(timeData, xMin, handles), smoothed);
}

/// The Resources chart's right-hand Page Faults axis (#1024).
[[nodiscard]] inline double faultAxisDataMax(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(seriesMax, {smoothed.pageFaultsPerSec});
}

[[nodiscard]] inline double faultAxisDataMax(std::span<const double> timeData,
                                             double xMin,
                                             std::span<const double> faults,
                                             const ProcessSmoothedUsage& smoothed) noexcept
{
    return faultAxisDataMax(UI::Widgets::maxOfSeriesSince(timeData, xMin, faults), smoothed);
}

/// The I/O chart's: Read and Write, without the bars while the counters are unread (#1110).
[[nodiscard]] inline double ioAxisDataMax(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(seriesMax,
                                          {UI::Widgets::currentIfAvailable(smoothed.ioAvailable, smoothed.ioReadBytesPerSec),
                                           UI::Widgets::currentIfAvailable(smoothed.ioAvailable, smoothed.ioWriteBytesPerSec)});
}

[[nodiscard]] inline double ioAxisDataMax(std::span<const double> timeData,
                                          double xMin,
                                          std::span<const double> read,
                                          std::span<const double> write,
                                          const ProcessSmoothedUsage& smoothed) noexcept
{
    return ioAxisDataMax(UI::Widgets::maxOfSeriesSince(timeData, xMin, read, write), smoothed);
}

/// The Network chart's: Sent and Received, without the bars while they are unattributed (#1110).
[[nodiscard]] inline double networkAxisDataMax(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(seriesMax,
                                          {UI::Widgets::currentIfAvailable(smoothed.networkAvailable, smoothed.netSentBytesPerSec),
                                           UI::Widgets::currentIfAvailable(smoothed.networkAvailable, smoothed.netRecvBytesPerSec)});
}

[[nodiscard]] inline double networkAxisDataMax(std::span<const double> timeData,
                                               double xMin,
                                               std::span<const double> sent,
                                               std::span<const double> received,
                                               const ProcessSmoothedUsage& smoothed) noexcept
{
    return networkAxisDataMax(UI::Widgets::maxOfSeriesSince(timeData, xMin, sent, received), smoothed);
}

/// The Power chart's.
[[nodiscard]] inline double powerAxisDataMax(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(seriesMax, {smoothed.powerWatts});
}

[[nodiscard]] inline double powerAxisDataMax(std::span<const double> timeData,
                                             double xMin,
                                             std::span<const double> power,
                                             const ProcessSmoothedUsage& smoothed) noexcept
{
    return powerAxisDataMax(UI::Widgets::maxOfSeriesSince(timeData, xMin, power), smoothed);
}

/// The GPU Memory chart's, without the bar while the GPU probe gave no reading (#1210).
[[nodiscard]] inline double gpuMemoryAxisDataMax(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(seriesMax,
                                          {UI::Widgets::currentIfAvailable(smoothed.gpuMemoryAvailable, smoothed.gpuMemoryBytes)});
}

[[nodiscard]] inline double gpuMemoryAxisDataMax(std::span<const double> timeData,
                                                 double xMin,
                                                 std::span<const double> gpuMemory,
                                                 const ProcessSmoothedUsage& smoothed) noexcept
{
    return gpuMemoryAxisDataMax(UI::Widgets::maxOfSeriesSince(timeData, xMin, gpuMemory), smoothed);
}

/// The NPU Memory chart's (#1869): Dedicated and Shared, without the bars while the GPU probe gave no
/// reading (the NPU figures come with the GPU ones).
[[nodiscard]] inline double npuMemoryAxisDataMax(double seriesMax, const ProcessSmoothedUsage& smoothed) noexcept
{
    return UI::Widgets::withCurrentValues(seriesMax,
                                          {UI::Widgets::currentIfAvailable(smoothed.npuMemoryAvailable, smoothed.npuDedicatedMemoryBytes),
                                           UI::Widgets::currentIfAvailable(smoothed.npuMemoryAvailable, smoothed.npuSharedMemoryBytes)});
}

[[nodiscard]] inline double npuMemoryAxisDataMax(std::span<const double> timeData,
                                                 double xMin,
                                                 std::span<const double> dedicated,
                                                 std::span<const double> shared,
                                                 const ProcessSmoothedUsage& smoothed) noexcept
{
    return npuMemoryAxisDataMax(UI::Widgets::maxOfSeriesSince(timeData, xMin, dedicated, shared), smoothed);
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

/// What the NPU tab shows for @p proc and its retained @p history (#1869), decided as the GPU tab's
/// (gpuTabContentFor()) from the process's NPU figures and @p npuSupported (SampleRateReadings::
/// npuSupported, #1827): its usage and charts while it uses an NPU or
/// compute accelerator now or its retained history does, else why there is nothing to chart.
[[nodiscard]] inline GpuTabContent
npuTabContentFor(const Domain::ProcessSnapshot& proc, const ProcessDetailsHistory& history, bool npuSupported)
{
    const std::span<const double> util = history.series(ProcessSeries::NpuUtil);
    const std::span<const double> dedicated = history.series(ProcessSeries::NpuDedicatedMemory);
    const std::span<const double> shared = history.series(ProcessSeries::NpuSharedMemory);
    return gpuTabContent(npuSupported,
                         hasGpuUsageToShow(proc.npuMemoryBytes, proc.npuUtilPercent, !proc.npuDevices.empty(), util, dedicated, shared),
                         hasAnyReading(util) || hasAnyReading(dedicated) || hasAnyReading(shared));
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
