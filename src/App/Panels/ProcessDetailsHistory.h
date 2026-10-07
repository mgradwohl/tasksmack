#pragma once

// The selected process's chart history in Process Details: one timestamp axis and every per-process
// series kept on it, appended, trimmed and cleared together (#1179). Extracted from
// ProcessDetailsPanel so it is unit-testable without a live ImGui context (ProcessDetailsPanel.cpp is
// not linked into the tests), following CONTRIBUTING.md's "extract the pure decision logic into a
// small header" pattern.
//
// The panel used to keep eighteen parallel vectors in step by hand, so adding a series meant editing
// a push, a gap push, a trim and a clear; missing one misaligned that series with the time axis. Here
// the series live in one private array, and append(), trim() and clear() are the only ways to change
// them, so they cannot drift apart.

#include "Domain/History.h"
#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "ProcessDetailsPanel_HistoryHelpers.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace App::Detail
{

/// Each per-process series Process Details charts, in storage order.
enum class ProcessSeries : std::uint8_t
{
    CpuTotal,     ///< CPU % of the machine
    CpuUser,      ///< User-mode CPU %
    CpuSystem,    ///< Kernel-mode CPU %
    MemoryUsed,   ///< Resident (used) bytes (#1195)
    MemoryShared, ///< Shared bytes, best effort (#1195)
    Virtual,      ///< Virtual bytes (#992)
    Threads,      ///< Thread count
    Handles,      ///< Handle / file descriptor count; NaN where unread (#1110)
    PageFaults,   ///< Page faults per second
    IoRead,       ///< Disk read bytes per second; NaN where not a reading (#1210)
    IoWrite,      ///< Disk write bytes per second; NaN where not a reading (#1210)
    NetSent,      ///< Network send bytes per second; NaN where not a reading (#1210)
    NetReceived,  ///< Network receive bytes per second; NaN where not a reading (#1210)
    Power,        ///< Watts
    GpuUtil,      ///< GPU utilization %; NaN where the GPU probe gave none (#1210)
    GpuMemory,    ///< GPU memory bytes; NaN where the GPU probe gave none (#1210)
    GdiObjects,   ///< GDI object count (Windows); NaN where unread (#1148)
    Count,
};

inline constexpr std::size_t PROCESS_SERIES_COUNT = static_cast<std::size_t>(ProcessSeries::Count);

/// One history point's value for every series, by name. A value the probe could not read is NaN,
/// drawn as a gap (#1110). Stored as double to avoid narrowing; converted only at the ImPlot boundary.
struct ProcessHistoryPoint
{
    double cpuTotal = 0.0;
    double cpuUser = 0.0;
    double cpuSystem = 0.0;
    double memoryUsed = 0.0;
    double memoryShared = 0.0;
    double virtualBytes = 0.0;
    double threads = 0.0;
    double handles = 0.0;
    double pageFaults = 0.0;
    double ioRead = 0.0;
    double ioWrite = 0.0;
    double netSent = 0.0;
    double netReceived = 0.0;
    double power = 0.0;
    double gpuUtil = 0.0;
    double gpuMemory = 0.0;
    double gdiObjects = 0.0;
};

/// Which ProcessHistoryPoint field feeds each ProcessSeries, indexed by the series. A plain table
/// rather than a switch, so a series added to the enum without a field here fails the checks below.
inline constexpr std::array<double ProcessHistoryPoint::*, PROCESS_SERIES_COUNT> PROCESS_SERIES_FIELDS{
    &ProcessHistoryPoint::cpuTotal,
    &ProcessHistoryPoint::cpuUser,
    &ProcessHistoryPoint::cpuSystem,
    &ProcessHistoryPoint::memoryUsed,
    &ProcessHistoryPoint::memoryShared,
    &ProcessHistoryPoint::virtualBytes,
    &ProcessHistoryPoint::threads,
    &ProcessHistoryPoint::handles,
    &ProcessHistoryPoint::pageFaults,
    &ProcessHistoryPoint::ioRead,
    &ProcessHistoryPoint::ioWrite,
    &ProcessHistoryPoint::netSent,
    &ProcessHistoryPoint::netReceived,
    &ProcessHistoryPoint::power,
    &ProcessHistoryPoint::gpuUtil,
    &ProcessHistoryPoint::gpuMemory,
    &ProcessHistoryPoint::gdiObjects,
};

// Every series has a field, and every field a series: a missing table entry is a null member pointer,
// and a field with no series makes the point larger than one double per series.
static_assert(std::ranges::none_of(PROCESS_SERIES_FIELDS, [](double ProcessHistoryPoint::* field) { return field == nullptr; }),
              "every ProcessSeries needs its ProcessHistoryPoint field in PROCESS_SERIES_FIELDS");
static_assert(sizeof(ProcessHistoryPoint) == PROCESS_SERIES_COUNT * sizeof(double),
              "every ProcessHistoryPoint field needs a ProcessSeries");

/// The history point for @p snapshot, with @p rateReadings saying which of its I/O, network and GPU
/// figures are readings, by the sample's own generation (rateReadings()); the others are gaps.
[[nodiscard]] inline ProcessHistoryPoint historyPointFrom(const Domain::ProcessSnapshot& snapshot, SampleRateReadings rateReadings) noexcept
{
    using Domain::Numeric::toDouble;
    return {
        .cpuTotal = snapshot.cpuPercent,
        .cpuUser = snapshot.cpuUserPercent,
        .cpuSystem = snapshot.cpuSystemPercent,
        // Bytes, not a percent of RAM: a typical process is under 1 % of RAM, which drew a flat line
        // on a 0-100 % axis (#1195). The share of RAM is shown in the tooltip and bar text instead.
        .memoryUsed = toDouble(snapshot.memoryBytes),
        .memoryShared = toDouble(snapshot.sharedBytes),
        // Bytes, not a percent of RAM: a process's virtual size is usually larger than physical RAM,
        // so as a percent it was clamped to 100 and carried no information (#992).
        .virtualBytes = toDouble(snapshot.virtualBytes),
        .threads = toDouble(snapshot.threadCount),
        .handles = readingOrGap(snapshot.handleCountAvailable, toDouble(snapshot.handleCount)),
        .pageFaults = snapshot.pageFaultsPerSec,
        // Gaps too where the probe has no such counters at all, not a line of measured-looking zeros (#1210).
        .ioRead = readingOrGap(rateReadings.io, snapshot.ioReadBytesPerSec),
        .ioWrite = readingOrGap(rateReadings.io, snapshot.ioWriteBytesPerSec),
        .netSent = readingOrGap(rateReadings.network, snapshot.netSentBytesPerSec),
        .netReceived = readingOrGap(rateReadings.network, snapshot.netReceivedBytesPerSec),
        .power = snapshot.powerWatts,
        // Gaps where the GPU probe supplied no per-process data, or memory but not utilization (NVML on
        // Linux), when this sample's generation was produced -- not as of the latest frame (#1210).
        .gpuUtil = readingOrGap(rateReadings.gpuUtilization, snapshot.gpuUtilPercent),
        .gpuMemory = readingOrGap(rateReadings.gpuPerProcess, toDouble(snapshot.gpuMemoryBytes)),
        // NaN signals "no data" to the plot; ImPlot renders NaN as a gap in the line.
        .gdiObjects = snapshot.gdiObjectCount.has_value() ? toDouble(*snapshot.gdiObjectCount) : std::numeric_limits<double>::quiet_NaN(),
    };
}

/// The selected process's history: a timestamp axis (seconds, oldest first) and one value per
/// ProcessSeries at each timestamp. Every series always has exactly size() values.
///
/// Vectors, not deques or rings: the charts plot the newest samples in place through spans, where a
/// deque had to be copied out every frame (#1018). Trimming erases from the front, once per sample,
/// not per frame.
class ProcessDetailsHistory
{
  public:
    /// Appends a point at @p timeSeconds. When @p gapBefore (generations were published that are no
    /// longer available, see takeSamples()) and @p timeSeconds is after the newest point, a gap point
    /// goes first, midway between them: NaN in every series, so each chart shows a gap rather than a
    /// line drawn across the missing samples.
    void append(double timeSeconds, const ProcessHistoryPoint& point, bool gapBefore)
    {
        if (gapBefore && !m_Timestamps.empty() && timeSeconds > m_Timestamps.back())
        {
            m_Timestamps.push_back((m_Timestamps.back() + timeSeconds) * 0.5);
            for (std::vector<double>& series : m_Series)
            {
                series.push_back(std::numeric_limits<double>::quiet_NaN());
            }
        }
        m_Timestamps.push_back(timeSeconds);
        for (std::size_t i = 0; i < PROCESS_SERIES_COUNT; ++i)
        {
            m_Series[i].push_back(point.*PROCESS_SERIES_FIELDS[i]);
        }
    }

    /// Drops the points older than @p windowSeconds before the newest point -- but keeps the newest
    /// point before that cutoff, so the charts' lines run off the window's left edge instead of leaving
    /// an empty strip there after every trim (#1016), unless it is across a gap
    /// (Domain::HistoryUtils::trimCountBefore()). Nothing happens when empty.
    void trimToWindow(double windowSeconds)
    {
        if (m_Timestamps.empty())
        {
            return;
        }
        const double cutoff = m_Timestamps.back() - windowSeconds;
        const std::size_t removeCount = Domain::HistoryUtils::trimCountBefore(m_Timestamps, cutoff);
        if (removeCount == 0)
        {
            return;
        }
        // One erase per buffer rather than a pop_front per sample: vector erase from the front shifts
        // the rest, so it must not run once per dropped sample.
        dropOldest(m_Timestamps, removeCount);
        for (std::vector<double>& series : m_Series)
        {
            dropOldest(series, removeCount);
        }
    }

    /// Empties the axis and every series (a new selection, including a reused PID).
    void clear() noexcept
    {
        m_Timestamps.clear();
        for (std::vector<double>& series : m_Series)
        {
            series.clear();
        }
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_Timestamps.empty();
    }

    /// The number of points, the same for the axis and every series.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Timestamps.size();
    }

    /// The newest point's time. Requires !empty().
    [[nodiscard]] double newestTimeSeconds() const noexcept
    {
        return m_Timestamps.back();
    }

    [[nodiscard]] std::span<const double> timestamps() const noexcept
    {
        return m_Timestamps;
    }

    [[nodiscard]] std::span<const double> series(ProcessSeries which) const noexcept
    {
        return m_Series[static_cast<std::size_t>(which)];
    }

  private:
    static void dropOldest(std::vector<double>& data, std::size_t count)
    {
        data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(std::min(count, data.size())));
    }

    std::vector<double> m_Timestamps;
    std::array<std::vector<double>, PROCESS_SERIES_COUNT> m_Series;
};

} // namespace App::Detail
