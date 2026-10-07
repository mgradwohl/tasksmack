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
#include <type_traits>
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

// The table maps series to fields one-to-one. A missing entry is a null member pointer; a field
// listed twice is a duplicate entry; and with no nulls or duplicates, the table names
// PROCESS_SERIES_COUNT distinct fields, so the size check leaves no field without a series.
static_assert(std::ranges::none_of(PROCESS_SERIES_FIELDS, [](double ProcessHistoryPoint::* field) { return field == nullptr; }),
              "every ProcessSeries needs its ProcessHistoryPoint field in PROCESS_SERIES_FIELDS");
static_assert(
    []
    {
        for (std::size_t i = 0; i < PROCESS_SERIES_FIELDS.size(); ++i)
        {
            for (std::size_t j = i + 1; j < PROCESS_SERIES_FIELDS.size(); ++j)
            {
                if (PROCESS_SERIES_FIELDS[i] == PROCESS_SERIES_FIELDS[j])
                {
                    return false;
                }
            }
        }
        return true;
    }(),
    "each ProcessHistoryPoint field may feed only one ProcessSeries in PROCESS_SERIES_FIELDS");
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
/// Vectors, not deques or rings: the charts plot the newest samples in place through spans, and
/// ImPlot needs each series contiguous, where a deque had to be copied out every frame (#1018).
///
/// Trimming is lazy (#1179): trimToWindow() only advances a logical start offset shared by the axis
/// and every series, and the points before it stay in the buffers, unread, until they are at least as
/// many as the live points. Only then are they erased, in one pass per buffer. Erasing from the
/// front on every sample used to shift the whole window of all eighteen buffers once per sample --
/// about 4 MB/s of memmove at 100 ms over the default 5 minutes, six times that at 30 minutes. Now
/// each live point is moved about once per window's worth of samples, so the cost per sample is
/// amortized O(1). The reads (timestamps(), series()) return spans that start at the logical start,
/// so a reader cannot tell a compacted history from one that has not been compacted yet.
///
/// Memory: each buffer holds fewer than twice the live points (the trimmed prefix is compacted
/// before it reaches the live count), and its capacity is held to about twice the most live points
/// it has had (see append()) rather than growing by doubling, which could take it to nearly four
/// times the window. At the largest window and fastest interval (30 minutes at 100 ms) that is
/// 18,000 points x 18 buffers x 8 bytes, about 2.6 MB live and about 5.2 MB allocated per Process
/// Details history, plus the old buffer briefly while one grows. Capacity is never released, so a
/// shorter window or clear() keeps the larger allocation for reuse.
class ProcessDetailsHistory
{
  public:
    /// Appends a point at @p timeSeconds. When @p gapBefore (generations were published that are no
    /// longer available, see takeSamples()) and @p timeSeconds is after the newest point, a gap point
    /// goes first, midway between them: NaN in every series, so each chart shows a gap rather than a
    /// line drawn across the missing samples.
    ///
    /// Strong exception guarantee: every buffer gets room for the new points before any is pushed, so
    /// an allocation failure (std::bad_alloc from reserveAtLeast()) leaves size() and every read as they were, and the
    /// axis and series stay the same length. The push_backs after it cannot throw: they fit in the
    /// reserved capacity, and copying a double does not throw.
    ///
    /// When the new points do not fit, the trimmed prefix is compacted first (compact() does not
    /// throw and leaves every read as it was, so the guarantee holds), and only then does a buffer
    /// grow, to twice the live points. A reallocation copies only live points, and the capacity stays
    /// within about twice the most live points the history has had. Growing to twice the live count
    /// right after a compaction leaves room for at least as many appends again before the next
    /// growth, so the cost stays amortized O(1).
    void append(double timeSeconds, const ProcessHistoryPoint& point, bool gapBefore)
    {
        const bool addGap = gapBefore && !empty() && timeSeconds > m_Timestamps.back();
        const std::size_t addCount = addGap ? 2U : 1U;
        if (m_Timestamps.size() + addCount > m_Timestamps.capacity())
        {
            compact();
            const std::size_t newCapacity = 2 * (size() + addCount);
            reserveAtLeast(m_Timestamps, newCapacity);
            for (std::vector<double>& series : m_Series)
            {
                reserveAtLeast(series, newCapacity);
            }
        }

        if (addGap)
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
    ///
    /// The dropped points leave the reads at once; their storage is reclaimed lazily (see the class
    /// comment). Does not throw: advancing the start allocates nothing, and compact() erases from
    /// vectors of double, which allocates nothing and moves doubles, so every buffer drops the same count.
    void trimToWindow(double windowSeconds) noexcept
    {
        if (empty())
        {
            return;
        }
        const double cutoff = m_Timestamps.back() - windowSeconds;
        const std::size_t removeCount = Domain::HistoryUtils::trimCountBefore(timestamps(), cutoff);
        if (removeCount == 0)
        {
            return;
        }
        m_Start += std::min(removeCount, size());
        // Compact once the dead prefix is as long as the live points: each compaction then moves at
        // most as many points as trims dropped since the last one, so the moves cost O(1) per point.
        if (m_Start >= size())
        {
            compact();
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
        m_Start = 0;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size() == 0;
    }

    /// The number of points, the same for the axis and every series.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Timestamps.size() - m_Start;
    }

    /// The newest point's time. Requires !empty().
    [[nodiscard]] double newestTimeSeconds() const noexcept
    {
        return m_Timestamps.back();
    }

    /// The axis from the oldest point kept to the newest, contiguous. Valid until the next append(),
    /// trimToWindow() or clear().
    [[nodiscard]] std::span<const double> timestamps() const noexcept
    {
        return std::span<const double>(m_Timestamps).subspan(m_Start);
    }

    /// One series from the oldest point kept to the newest, contiguous and aligned with timestamps().
    /// Valid until the next append(), trimToWindow() or clear().
    [[nodiscard]] std::span<const double> series(ProcessSeries which) const noexcept
    {
        return std::span<const double>(m_Series[static_cast<std::size_t>(which)]).subspan(m_Start);
    }

    /// The axis buffer's capacity in points; every series buffer grows with it. Exposed so the tests
    /// can check the memory bound in the class comment.
    [[nodiscard]] std::size_t storageCapacity() const noexcept
    {
        return m_Timestamps.capacity();
    }

    /// How many trimmed points are still held in the buffers ahead of the live ones, waiting for the
    /// next compaction. Never part of a read; exposed so the tests and the benchmark can see when
    /// storage is reclaimed.
    [[nodiscard]] std::size_t trimmedPrefixSize() const noexcept
    {
        return m_Start;
    }

  private:
    static_assert(std::is_nothrow_copy_constructible_v<double> && std::is_nothrow_move_assignable_v<double>,
                  "append() and trimToWindow() rely on copying and moving values not throwing");

    /// Makes room for @p capacity values in @p data. Only the capacity changes: a throw leaves
    /// @p data's size and values as they were.
    static void reserveAtLeast(std::vector<double>& data, std::size_t capacity)
    {
        if (capacity > data.capacity())
        {
            data.reserve(capacity);
        }
    }

    /// Erases the trimmed prefix from every buffer, one erase each, so the live points start at index
    /// 0. What the reads return is unchanged (only where it lives), and no capacity is released, so
    /// the next appends need no allocation either.
    void compact() noexcept
    {
        dropOldest(m_Timestamps, m_Start);
        for (std::vector<double>& series : m_Series)
        {
            dropOldest(series, m_Start);
        }
        m_Start = 0;
    }

    static void dropOldest(std::vector<double>& data, std::size_t count) noexcept
    {
        data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(std::min(count, data.size())));
    }

    std::vector<double> m_Timestamps;
    std::array<std::vector<double>, PROCESS_SERIES_COUNT> m_Series;
    std::size_t m_Start = 0; ///< The oldest live point's index in every buffer; the ones before it are trimmed
};

} // namespace App::Detail
