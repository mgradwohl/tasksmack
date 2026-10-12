#pragma once

// Recent per-process history for the Processes table's sparklines (#1612).
//
// A fixed ring of the latest RING_CAPACITY values per process per metric, filled by ProcessModel when
// it publishes a generation (no extra sampling) and published with it, immutable, row-aligned with
// that generation's snapshot vector: the UI finds a row's ring by the row's index, with no lookup or
// lock. Kept only while at least one metric is asked for (ProcessModel::setRowHistoryMetrics()), so
// the default -- no sparkline -- keeps no history at all.

#include "ProcessSnapshot.h"

#include <array>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Domain::ProcessRowHistory
{

/// How many values a ring keeps: 30 refreshes, 30 s at the default 1 s interval.
inline constexpr std::size_t RING_CAPACITY = 30;

/// The per-process values a ring can follow: the Processes table's meter columns.
enum class Metric : std::uint8_t
{
    CpuPercent,
    MemPercent,
    Resident,
    IoRead,
    IoWrite,
    NetSent,
    NetReceived,
    GpuPercent,
    GpuMemory,
};

inline constexpr std::size_t METRIC_COUNT = 9;

/// A set of metrics, one bit per Metric.
using MetricSet = std::uint16_t;

[[nodiscard]] constexpr MetricSet bitOf(Metric metric) noexcept
{
    return static_cast<MetricSet>(1U << static_cast<unsigned>(metric));
}

[[nodiscard]] constexpr bool contains(MetricSet metrics, Metric metric) noexcept
{
    return (metrics & bitOf(metric)) != 0;
}

/// The value @p metric has for @p proc; 0 for a value the probe could not read.
[[nodiscard]] inline double valueOf(const ProcessSnapshot& proc, Metric metric) noexcept
{
    switch (metric)
    {
    case Metric::CpuPercent:
        return proc.cpuPercent;
    case Metric::MemPercent:
        return proc.memoryPercent;
    case Metric::Resident:
        return static_cast<double>(proc.memoryBytes);
    case Metric::IoRead:
        return proc.ioAvailable ? proc.ioReadBytesPerSec : 0.0;
    case Metric::IoWrite:
        return proc.ioAvailable ? proc.ioWriteBytesPerSec : 0.0;
    case Metric::NetSent:
        return proc.networkAvailable ? proc.netSentBytesPerSec : 0.0;
    case Metric::NetReceived:
        return proc.networkAvailable ? proc.netReceivedBytesPerSec : 0.0;
    case Metric::GpuPercent:
        return proc.gpuFieldsRead ? proc.gpuUtilPercent : 0.0;
    case Metric::GpuMemory:
        return proc.gpuFieldsRead ? static_cast<double>(proc.gpuMemoryBytes) : 0.0;
    }
    return 0.0;
}

/// The latest RING_CAPACITY values of one metric of one process, oldest first. Never allocates.
class Ring
{
  public:
    /// Appends @p value, dropping the oldest once full. A value that is not finite is kept as 0.
    void push(float value) noexcept
    {
        const float kept = std::isfinite(value) ? value : 0.0F;
        if (m_Count < RING_CAPACITY)
        {
            m_Values[(m_Start + m_Count) % RING_CAPACITY] = kept;
            ++m_Count;
            return;
        }
        m_Values[m_Start] = kept;
        m_Start = static_cast<std::uint8_t>((m_Start + 1U) % RING_CAPACITY);
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_Count;
    }

    /// The value @p index places from the oldest kept; @p index must be below size().
    [[nodiscard]] float at(std::size_t index) const noexcept
    {
        return m_Values[(m_Start + index) % RING_CAPACITY];
    }

    /// The newest value, or 0 when empty.
    [[nodiscard]] float latest() const noexcept
    {
        return m_Count == 0 ? 0.0F : at(m_Count - 1U);
    }

  private:
    std::array<float, RING_CAPACITY> m_Values{};
    std::uint8_t m_Start = 0; ///< The oldest value's slot
    std::uint8_t m_Count = 0;
};

/// One published generation's rings, row-aligned with that generation's snapshot vector: row i's
/// rings belong to snapshots[i]. Immutable once published.
class Generation
{
  public:
    Generation(MetricSet metrics, std::size_t rows);

    [[nodiscard]] MetricSet metrics() const noexcept
    {
        return m_Metrics;
    }

    [[nodiscard]] std::size_t rowCount() const noexcept
    {
        return m_Rows;
    }

    /// Row @p row's ring of @p metric, or nullptr when that metric is not kept or the row is not in
    /// this generation.
    [[nodiscard]] const Ring* ring(std::size_t row, Metric metric) const noexcept;

    /// The bytes the rings take.
    [[nodiscard]] std::size_t ringBytes() const noexcept
    {
        return m_Rings.size() * sizeof(Ring);
    }

  private:
    friend class Tracker;

    static constexpr std::uint8_t NO_SLOT = 0xFF;

    [[nodiscard]] Ring& ringAt(std::size_t row, std::size_t slot) noexcept
    {
        return m_Rings[(row * m_SlotsPerRow) + slot];
    }

    MetricSet m_Metrics = 0;
    std::size_t m_Rows = 0;
    std::size_t m_SlotsPerRow = 0;
    std::array<std::uint8_t, METRIC_COUNT> m_SlotOf{}; ///< Each metric's slot in a row, or NO_SLOT
    std::vector<Ring> m_Rings;                         ///< Row-major: m_SlotsPerRow rings a row
};

/// The writer's side: carries each process's rings from one generation to the next, by uniqueKey,
/// and drops a process's rings with the first generation that no longer lists it. Not thread-safe:
/// ProcessModel calls it under its sampling lock; readers only ever see the Generations it returns.
class Tracker
{
  public:
    /// The generation after the last one for @p snapshots, with @p metrics kept: each process's rings
    /// as the last generation had them (a metric just asked for starts empty), plus its value now.
    /// With no metrics, drops every ring and returns nullptr: nothing is kept while nothing is shown.
    /// Allocates only the new generation once its key index has grown to the process count.
    [[nodiscard]] std::shared_ptr<const Generation> advance(const std::vector<ProcessSnapshot>& snapshots, MetricSet metrics);

    /// The rows the last generation has (0 once dropped).
    [[nodiscard]] std::size_t rowCount() const noexcept
    {
        return m_Last ? m_Last->rowCount() : 0;
    }

    /// The bytes held: the last generation's rings and the uniqueKey index. 0 once dropped.
    [[nodiscard]] std::size_t bytes() const noexcept;

  private:
    /// A uniqueKey and its row in a generation.
    struct KeyRow
    {
        std::uint64_t key = 0;
        std::size_t row = 0;

        friend auto operator<=>(const KeyRow&, const KeyRow&) = default;
    };

    std::shared_ptr<const Generation> m_Last;
    std::vector<KeyRow> m_LastKeys; ///< m_Last's rows by uniqueKey, sorted: the index the next generation looks up
    std::vector<KeyRow> m_NextKeys; ///< Scratch for the next generation's index, swapped with m_LastKeys
};

} // namespace Domain::ProcessRowHistory
