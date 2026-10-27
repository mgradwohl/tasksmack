#pragma once

// Process Details' collapsible Threads section (#3069): the selected process's threads in a TID / NAME /
// STATE / CPU % / CPU TIME / NICE table, sortable by any column (by CPU % by default), filterable by
// name or TID, with the count in the section's header.
//
// The read cadence is the Modules section's (ProcessModulesView.h), at the Settings refresh interval:
// CPU % is a live figure, worked out here from two reads' CPU times (the first read shows the dash).
// update() starts a read only while the section was drawn open on the last frame, on a worker thread.
// render() only draws what the last read returned, formatted once per read.
//
// Everything but render() is defined here, free of ImGui, so the cadence, the CPU %, the statuses, the
// sort and the filter are tested against a mock reader without an ImGui context.

#include "Domain/ProcessState.h"
#include "Domain/SamplingConfig.h"
#include "LazyBackgroundRead.h"
#include "Platform/AsciiCase.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessThreads.h"
#include "Platform/ThreadName.h"
#include "UI/Format.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace App
{

namespace Detail
{

/// Rows the table shows before it scrolls inside the section.
inline constexpr std::size_t THREADS_TABLE_MAX_VISIBLE_ROWS = 12;

/// A thread's CPU % before a second read gives it one: the table's unavailable dash.
inline constexpr std::string_view THREAD_CPU_UNKNOWN = "\xE2\x80\x94";

/// The line shown in place of the table for a read that did not succeed; empty for Ok.
[[nodiscard]] constexpr std::string_view threadsStatusText(Platform::ThreadsReadStatus status) noexcept
{
    switch (status)
    {
    case Platform::ThreadsReadStatus::Ok:
        return {};
    case Platform::ThreadsReadStatus::PermissionDenied:
        return "Access denied: another user's process";
    case Platform::ThreadsReadStatus::ProcessExited:
        return "Process exited";
    case Platform::ThreadsReadStatus::Unsupported:
        return "Not available on this platform";
    case Platform::ThreadsReadStatus::IdentityUnknown:
        return "Not available for this process"; // retried at the next refresh, in case the start time arrives
    case Platform::ThreadsReadStatus::Failed:
        break;
    }
    return "Could not be read";
}

/// The table's columns, also its sort keys (the ImGui column user IDs).
enum class ThreadsColumn : std::uint8_t
{
    Tid,
    Name,
    State,
    Cpu,
    CpuTime,
    Nice,
};

/// A thread's CPU use between two reads, as a share of the whole machine like the Processes table's CPU %:
/// @p deltaTicks of CPU time (at @p ticksPerSecond) over @p elapsedSeconds on @p cpuCount CPUs. nullopt
/// when there is nothing to divide by.
[[nodiscard]] constexpr std::optional<double>
threadCpuPercent(std::uint64_t deltaTicks, std::int64_t ticksPerSecond, double elapsedSeconds, std::size_t cpuCount) noexcept
{
    if (ticksPerSecond <= 0 || elapsedSeconds <= 0.0 || cpuCount == 0)
    {
        return std::nullopt;
    }
    const double percent =
        (static_cast<double>(deltaTicks) / static_cast<double>(ticksPerSecond)) / (elapsedSeconds * static_cast<double>(cpuCount)) * 100.0;
    return std::clamp(percent, 0.0, 100.0);
}

} // namespace Detail

/// The Threads section for the process Process Details shows.
class ProcessThreadsView
{
  public:
    /// One thread as the table shows it: the raw row (for sorting) and its text, formatted once per read.
    struct Row
    {
        Platform::ProcessThread thread;
        std::optional<double> cpuPercent; ///< None on the first read of it
        double cpuSeconds = 0.0;
        std::string tid;
        std::string state;   ///< "Sleeping"
        std::string cpu;     ///< "3.2%", or the dash
        std::string cpuTime; ///< "1m 04s"
        std::string nice;    ///< "0"
    };

    /// @p cpuCount is what CPU % is a share of (the machine's CPUs, as the Processes table's CPU %).
    explicit ProcessThreadsView(std::size_t cpuCount = 1) : m_CpuCount(std::max<std::size_t>(cpuCount, 1))
    {}

    /// Draws the section: a collapsing "Threads (N)" header and, while it is open, the filter box and
    /// table, or the last read's status line. Draws nothing when @p hasThreads is false.
    void render(bool hasThreads);

    /// The panel's per-frame update, as ProcessModulesView::update(), at @p refreshMs (the Settings
    /// interval). Never blocks; @p nowSeconds is a steady clock, for CPU %. The caller must keep
    /// @p reader alive until a read in flight finishes (the destructor waits for it).
    /// @return Whether it started a read.
    bool update(Platform::IProcessThreadsReader* reader,
                const Platform::ProcessTarget& target,
                float deltaSeconds,
                int refreshMs,
                double nowSeconds)
    {
        m_Read.setRefreshMs(refreshMs);
        if (auto result = m_Read.takeFinished(target, false))
        {
            applyResult(*result, nowSeconds);
        }
        if (!m_Read.due(deltaSeconds) || reader == nullptr || !reader->hasThreads())
        {
            return false;
        }
        if (auto failed = m_Read.start(target, [reader](const Platform::ProcessTarget& t) { return reader->readThreads(t); }))
        {
            applyResult(*failed, nowSeconds);
        }
        return true;
    }

    /// Waits for the read in flight, if any, and takes it in as update() would (tests only).
    void finishPendingRead(const Platform::ProcessTarget& target, double nowSeconds)
    {
        if (auto result = m_Read.takeFinished(target, true))
        {
            applyResult(*result, nowSeconds);
        }
    }

    /// A different process was selected: drop its threads and read afresh when next shown. The sort and
    /// the filter are viewing preferences and stay.
    void onSelectionChanged() noexcept
    {
        m_Read.reset();
        m_HasRead = false;
        m_Status = Platform::ThreadsReadStatus::Ok;
        m_Detail.clear();
        m_Truncated = false;
        m_Rows.clear();
        m_Previous.clear();
        m_PreviousAt.reset();
        m_FilteredRows.clear();
        m_FilterDirty = true;
    }

    /// Takes in a read made at @p nowSeconds: its status and, for Ok, its threads formatted and sorted,
    /// each one's CPU % from its CPU time at the read before (the same thread: same TID and start time).
    void applyResult(const Platform::ThreadsReadResult& result, double nowSeconds)
    {
        m_HasRead = true;
        m_Status = result.status;
        m_Detail = result.detail;
        m_Truncated = result.truncated;
        m_Rows.clear();
        if (result.status != Platform::ThreadsReadStatus::Ok)
        {
            m_Previous.clear();
            m_PreviousAt.reset();
            m_FilterDirty = true;
            return;
        }
        const double elapsed = m_PreviousAt.has_value() ? nowSeconds - *m_PreviousAt : 0.0;
        const double ticksPerSecond = static_cast<double>(std::max<std::int64_t>(result.ticksPerSecond, 1));
        std::unordered_map<std::int32_t, Sample> current;
        current.reserve(result.threads.size());
        m_Rows.reserve(result.threads.size());
        for (const Platform::ProcessThread& thread : result.threads)
        {
            const std::uint64_t ticks = thread.userTicks + thread.systemTicks;
            std::optional<double> cpu;
            if (const auto previous = m_Previous.find(thread.tid);
                previous != m_Previous.end() && previous->second.startTicks == thread.startTicks && ticks >= previous->second.ticks)
            {
                cpu = Detail::threadCpuPercent(ticks - previous->second.ticks, result.ticksPerSecond, elapsed, m_CpuCount);
            }
            current.emplace(thread.tid, Sample{.ticks = ticks, .startTicks = thread.startTicks});
            const double cpuSeconds = static_cast<double>(ticks) / ticksPerSecond;
            m_Rows.push_back({.thread = thread,
                              .cpuPercent = cpu,
                              .cpuSeconds = cpuSeconds,
                              .tid = std::to_string(thread.tid),
                              .state = std::string(Domain::processStateName(thread.state)),
                              .cpu = cpu.has_value() ? UI::Format::formatPercent(*cpu) : std::string(Detail::THREAD_CPU_UNKNOWN),
                              .cpuTime = UI::Format::formatDuration(cpuSeconds),
                              .nice = std::to_string(thread.nice)});
        }
        m_Previous = std::move(current);
        m_PreviousAt = nowSeconds;
        sortRows();
    }

    void markDrawnOpen() noexcept
    {
        m_Read.markDrawnOpen();
    }

    /// Sorts by @p column, as clicking its header does. Ties fall back to the TID.
    void setSort(Detail::ThreadsColumn column, bool ascending)
    {
        m_SortColumn = column;
        m_SortAscending = ascending;
        sortRows();
    }

    /// Sets the filter text, as typing in the filter box does.
    void setFilter(std::string filter)
    {
        m_Filter = std::move(filter);
    }

    /// Indices into rows() whose name or TID contains the filter (ignoring case). Cached until the rows
    /// or the filter change.
    [[nodiscard]] std::span<const std::size_t> filteredRows()
    {
        if (m_FilterDirty || m_FilterKey != m_Filter)
        {
            m_FilteredRows.clear();
            for (std::size_t i = 0; i < m_Rows.size(); ++i)
            {
                if (Platform::containsIgnoreCase(m_Rows[i].thread.name, m_Filter) || m_Rows[i].tid.contains(m_Filter))
                {
                    m_FilteredRows.push_back(i);
                }
            }
            m_FilterKey = m_Filter;
            m_FilterDirty = false;
        }
        return m_FilteredRows;
    }

    [[nodiscard]] bool hasRead() const noexcept
    {
        return m_HasRead;
    }

    [[nodiscard]] Platform::ThreadsReadStatus status() const noexcept
    {
        return m_Status;
    }

    /// The last read's threads, in the current sort order.
    [[nodiscard]] std::span<const Row> rows() const noexcept
    {
        return m_Rows;
    }

  private:
    struct Sample
    {
        std::uint64_t ticks = 0;
        std::uint64_t startTicks = 0;
    };

    [[nodiscard]] static Platform::ThreadsReadResult failedRead(std::string detail)
    {
        return {.status = Platform::ThreadsReadStatus::Failed, .threads = {}, .detail = std::move(detail)};
    }

    [[nodiscard]] static std::partial_ordering compareOn(Detail::ThreadsColumn column, const Row& a, const Row& b) noexcept
    {
        switch (column)
        {
        case Detail::ThreadsColumn::Name:
            return Platform::compareIgnoreCase(a.thread.name, b.thread.name);
        case Detail::ThreadsColumn::State:
            return a.state <=> b.state;
        case Detail::ThreadsColumn::Cpu:
            return a.cpuPercent.value_or(-1.0) <=> b.cpuPercent.value_or(-1.0); // none sorts below 0 %
        case Detail::ThreadsColumn::CpuTime:
            return a.cpuSeconds <=> b.cpuSeconds;
        case Detail::ThreadsColumn::Nice:
            return a.thread.nice <=> b.thread.nice;
        case Detail::ThreadsColumn::Tid:
            break;
        }
        return a.thread.tid <=> b.thread.tid;
    }

    void sortRows()
    {
        const Detail::ThreadsColumn column = m_SortColumn;
        const bool ascending = m_SortAscending;
        std::ranges::stable_sort(m_Rows,
                                 [column, ascending](const Row& a, const Row& b)
                                 {
                                     if (const auto primary = compareOn(column, a, b); primary != 0)
                                     {
                                         return ascending ? primary < 0 : primary > 0;
                                     }
                                     return a.thread.tid < b.thread.tid;
                                 });
        m_FilterDirty = true;
    }

    void renderTable();

    std::size_t m_CpuCount = 1;
    bool m_HasRead = false; // a read was taken in since the selection changed
    Platform::ThreadsReadStatus m_Status = Platform::ThreadsReadStatus::Ok;
    std::string m_Detail;
    bool m_Truncated = false;
    std::vector<Row> m_Rows;
    std::unordered_map<std::int32_t, Sample> m_Previous; // the last read's CPU time per thread, for CPU %
    std::optional<double> m_PreviousAt;                  // when it was taken in
    Detail::ThreadsColumn m_SortColumn = Detail::ThreadsColumn::Cpu;
    bool m_SortAscending = false;
    std::string m_Filter;
    std::string m_FilterKey;
    std::vector<std::size_t> m_FilteredRows;
    bool m_FilterDirty = true;
    std::string m_CountLabel;            // the header with the count: "Threads (12)"
    std::size_t m_LabelCount = SIZE_MAX; // the count m_CountLabel shows

    // The read in flight and its cadence (the Settings interval, set by update()). Destroying the view
    // waits for at most one read.
    Detail::LazyBackgroundRead<Platform::ThreadsReadResult> m_Read{
        Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS, Platform::THREADS_READ_THREAD_NAME, &failedRead};
};

} // namespace App
