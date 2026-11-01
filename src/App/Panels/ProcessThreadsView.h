#pragma once

// Process Details' collapsible Threads section (#3069): the selected process's threads in a TID / NAME /
// STATE / CPU % / CPU TIME / NICE (Windows: PRIORITY) / STARTED table, sortable by any column (by CPU % by
// default), filterable by name or TID, with the count in the section's header. On Windows each row's menu
// offers Suspend, Resume and End, each behind a confirm (ThreadActionControl.h); Linux can't do them to one
// thread, so its rows have no menu.
//
// The read cadence is the Modules section's (ProcessModulesView.h), at the Settings refresh interval:
// CPU % is a live figure, worked out here from two reads' CPU times (the first read shows the dash), over the
// time between the two reads as the worker took them, never between the frames that took them in (#3337).
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
#include "ThreadActionControl.h"
#include "UI/Format.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
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

/// The STATE cell: the state's name, then a suspended thread's suspend count or a waiting one's wait reason
/// (Windows, #3069): "Stopped (suspend count 2)", "Sleeping (UserRequest)", "Running".
[[nodiscard]] inline std::string threadStateText(const Platform::ProcessThread& thread)
{
    std::string text(Domain::processStateName(thread.state));
    if (thread.suspendCount > 0)
    {
        text += std::format(" (suspend count {})", thread.suspendCount);
    }
    else if (!thread.waitReason.empty())
    {
        text += std::format(" ({})", thread.waitReason);
    }
    return text;
}

/// Whether the row menu offers Resume for @p thread: it is suspended (#3069).
[[nodiscard]] constexpr bool threadCanResume(const Platform::ProcessThread& thread) noexcept
{
    return thread.suspendCount > 0 || thread.state == 'T';
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
    Started, ///< When it started (#3069)
};

/// When a thread that started @p startTicks after boot did, as the Processes table's start time reads, from
/// the boot time the read gave; the dash when that is unknown.
[[nodiscard]] inline std::string threadStartedText(std::uint64_t startTicks, std::int64_t ticksPerSecond, std::uint64_t bootTimeEpoch)
{
    if (bootTimeEpoch == 0)
    {
        return std::string(THREAD_CPU_UNKNOWN);
    }
    return UI::Format::formatEpochDateTimeShort(bootTimeEpoch +
                                                (startTicks / static_cast<std::uint64_t>(std::max<std::int64_t>(ticksPerSecond, 1))));
}

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
        std::string state;   ///< "Sleeping", "Stopped (suspend count 1)"
        std::string cpu;     ///< "3.2%", or the dash
        std::string cpuTime; ///< "1m 04s"
        std::string nice;    ///< "0"
        std::string started; ///< When it started, or the dash (#3069)
    };

    /// The clock a read's acquisition time is taken from, in seconds; called on the worker (#3337). It must be on
    /// the readers' clock (Platform::threadsReadClockSeconds(), the default), as a reader may stamp its own.
    using Clock = std::function<double()>;

    /// @p cpuCount is what CPU % is a share of (the machine's CPUs, as the Processes table's CPU %).
    /// @p clock stamps each read a reader left unstamped as it returns, on the worker (threadsReadClockSeconds()
    /// when empty; tests pass a fake).
    explicit ProcessThreadsView(std::size_t cpuCount = 1, Clock clock = {})
        : m_CpuCount(std::max<std::size_t>(cpuCount, 1)), m_Clock(clock ? std::move(clock) : Clock(&Platform::threadsReadClockSeconds))
    {}

    /// Draws the section: a collapsing "Threads (N)" header and, while it is open, the filter box and
    /// table, or the last read's status line. Draws nothing when @p hasThreads is false.
    void render(bool hasThreads);

    /// The panel's per-frame update, as ProcessModulesView::update(), at @p refreshMs (the Settings
    /// interval). Never blocks. The caller must keep @p reader alive until a read in flight finishes (the
    /// destructor waits for it).
    /// @return Whether it started a read.
    bool update(Platform::IProcessThreadsReader* reader, const Platform::ProcessTarget& target, float deltaSeconds, int refreshMs)
    {
        m_Target = target;
        // A thread action's result (#3069); one that succeeded reads the threads again at once, for the new state.
        m_ActionControl.update(deltaSeconds);
        if (m_ActionControl.takeActed())
        {
            m_Read.rearm();
        }
        m_Read.setRefreshMs(refreshMs);
        if (auto result = m_Read.takeFinished(target, false))
        {
            applyResult(*result);
        }
        m_Read.setCanAuthorize((reader != nullptr) && reader->canReadAuthorized());
        // "Authenticate to read" was pressed (#3069): read as an administrator, now, on the worker.
        if (m_Read.takeAuthorizeRequest() && reader != nullptr && !m_Read.inFlight())
        {
            if (auto failed = m_Read.start(target,
                                           [reader, clock = m_Clock](const Platform::ProcessTarget& t, const std::stop_token& stopToken)
                                           { return stamped(reader->readAuthorized(t, stopToken), clock); }))
            {
                applyResult(*failed);
            }
            return true;
        }
        if (!m_Read.due(deltaSeconds) || reader == nullptr || !reader->hasThreads())
        {
            return false;
        }
        if (auto failed = m_Read.start(
                target, [reader, clock = m_Clock](const Platform::ProcessTarget& t) { return stamped(reader->readThreads(t), clock); }))
        {
            applyResult(*failed);
        }
        return true;
    }

    /// Whether the status line offers "Authenticate to read" (#3069): the read was refused for want of
    /// permission and the reader can read as an administrator.
    [[nodiscard]] bool offersAuthenticateToRead() const noexcept
    {
        return m_HasRead && m_Status == Platform::ThreadsReadStatus::PermissionDenied && m_Read.canAuthorize() && !m_Read.inFlight();
    }

    /// "Authenticate to read" was pressed: the next update() reads as an administrator.
    void requestAuthorizedRead() noexcept
    {
        m_Read.requestAuthorized();
    }

    /// Waits for the read in flight, if any, and takes it in as update() would (tests only).
    void finishPendingRead(const Platform::ProcessTarget& target)
    {
        if (auto result = m_Read.takeFinished(target, true))
        {
            applyResult(*result);
        }
    }

    /// A different process was selected: drop its threads and read afresh when next shown. The sort and
    /// the filter are viewing preferences and stay.
    void onSelectionChanged()
    {
        m_ActionControl.onSelectionChanged();
        m_MenuThread.reset();
        m_OpenRowMenu = false;
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

    /// Whether the rows offer Suspend, Resume and End (the actions' canActOnThreads: Windows), and the process's
    /// name the confirm and the result line give. Each frame, before render(); the name is copied only when it changes.
    void setThreadActions(bool canAct, std::string_view processName)
    {
        m_CanActOnThreads = canAct;
        if (m_ProcessName != processName)
        {
            m_ProcessName = processName;
        }
    }

    /// A row menu's item: @p action on @p thread of the process shown, its confirm opened (#3069).
    void requestThreadAction(const Platform::ProcessThread& thread, Platform::ThreadAction action)
    {
        m_ActionControl.request(PendingThreadAction{.target = m_Target,
                                                    .tid = thread.tid,
                                                    .threadCreateTime = thread.createTime,
                                                    .threadName = thread.name,
                                                    .processName = m_ProcessName,
                                                    .action = action});
    }

    /// Opens the row menu for the thread @p tid of the last read, as a right-click on its row does (tests).
    void openRowMenu(std::int32_t tid)
    {
        for (const Row& row : m_Rows)
        {
            if (row.thread.tid == tid)
            {
                m_MenuThread = row.thread;
                m_OpenRowMenu = true;
            }
        }
    }

    /// Draws a thread action's confirm while one is pending, from the panel's window, every frame (as the
    /// Actions block's is): so a section collapsed or scrolled away doesn't leave it pending. Closed unconfirmed
    /// when the process shown is no longer the one it was for (@p liveTarget). @p actions does what is confirmed.
    void renderConfirmation(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget);

    /// The thread actions' state (tests).
    [[nodiscard]] ThreadActionControl& actionControl() noexcept
    {
        return m_ActionControl;
    }

    /// Takes in a read: its status and, for Ok, its threads formatted and sorted, each one's CPU % from its CPU
    /// time at the read before (the same thread: same TID and start time), over the time between the two reads'
    /// acquisitions (result.acquiredAtSeconds), however late either was taken in (#3337). A read with no
    /// acquisition time, or one not after the last's, gives no CPU % (the dash).
    void applyResult(const Platform::ThreadsReadResult& result)
    {
        m_HasRead = true;
        m_Status = result.status;
        if (result.status == Platform::ThreadsReadStatus::Ok)
        {
            m_WindowsPriorities = result.windowsPriorities;
        }
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
        // Zero or negative (no time between the acquisitions, or either unknown): no CPU %, nothing divided.
        const double elapsed =
            (m_PreviousAt.has_value() && result.acquiredAtSeconds.has_value()) ? *result.acquiredAtSeconds - *m_PreviousAt : 0.0;
        const double ticksPerSecond = static_cast<double>(std::max<std::int64_t>(result.ticksPerSecond, 1));
        std::unordered_map<std::int32_t, Sample> current;
        current.reserve(result.threads.size());
        m_Rows.reserve(result.threads.size());
        for (const Platform::ProcessThread& thread : result.threads)
        {
            const std::uint64_t ticks = thread.userTicks + thread.systemTicks;
            std::optional<double> cpu;
            if (const auto previous = m_Previous.find(thread.tid); elapsed > 0.0 && previous != m_Previous.end() &&
                                                                   previous->second.startTicks == thread.startTicks &&
                                                                   ticks >= previous->second.ticks)
            {
                cpu = Detail::threadCpuPercent(ticks - previous->second.ticks, result.ticksPerSecond, elapsed, m_CpuCount);
            }
            current.emplace(thread.tid, Sample{.ticks = ticks, .startTicks = thread.startTicks});
            const double cpuSeconds = static_cast<double>(ticks) / ticksPerSecond;
            m_Rows.push_back({.thread = thread,
                              .cpuPercent = cpu,
                              .cpuSeconds = cpuSeconds,
                              .tid = std::to_string(thread.tid),
                              .state = Detail::threadStateText(thread),
                              .cpu = cpu.has_value() ? UI::Format::formatPercent(*cpu) : std::string(Detail::THREAD_CPU_UNKNOWN),
                              .cpuTime = UI::Format::formatDuration(cpuSeconds),
                              .nice = std::to_string(thread.nice),
                              .started = Detail::threadStartedText(thread.startTicks, result.ticksPerSecond, result.bootTimeEpoch)});
        }
        m_Previous = std::move(current);
        m_PreviousAt = result.acquiredAtSeconds;
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

    /// @p result, with its acquisition time from @p clock when the reader left it unset: on the worker, as the
    /// read returns, so a result taken in late still divides by when its counters were read (#3337).
    [[nodiscard]] static Platform::ThreadsReadResult stamped(Platform::ThreadsReadResult result, const Clock& clock)
    {
        if (!result.acquiredAtSeconds.has_value())
        {
            result.acquiredAtSeconds = clock();
        }
        return result;
    }

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
        case Detail::ThreadsColumn::Started:
            return a.thread.startTicks <=> b.thread.startTicks;
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
    /// The result line of the last thread action, with "Authenticate and retry" after a refusal (#3069).
    void renderActionResult();
    /// The row menu (#3069), after the table: Suspend, Resume, End.
    void renderRowMenu();

    // Ordered by alignment, largest first (clang-analyzer-optin.performance.Padding), the flags last.
    std::size_t m_CpuCount = 1;
    std::size_t m_LabelCount = SIZE_MAX; // the count m_CountLabel shows
    std::optional<double> m_PreviousAt;  // when the last read's CPU times were read (its acquisition time, #3337)
    Platform::ProcessTarget m_Target{};  // the process shown, as of the last update() (for the thread actions)
    std::vector<Row> m_Rows;
    std::vector<std::size_t> m_FilteredRows;
    std::string m_Detail;
    std::string m_Filter;
    std::string m_FilterKey;
    std::string m_CountLabel;                            // the header with the count: "Threads (12)"
    std::string m_ProcessName;                           // the process's name, for the thread actions' confirm and result line (#3069)
    std::unordered_map<std::int32_t, Sample> m_Previous; // the last read's CPU time per thread, for CPU %
    Clock m_Clock;                                       // stamps each read on the worker (#3337)

    // The read in flight and its cadence (the Settings interval, set by update()). Destroying the view
    // waits for at most one read.
    Detail::LazyBackgroundRead<Platform::ThreadsReadResult> m_Read{
        Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS, Platform::THREADS_READ_THREAD_NAME, &failedRead};

    // The thread actions (#3069, Windows): the row menu's thread, its confirm and result line.
    std::optional<Platform::ProcessThread> m_MenuThread; // the row the menu is up for, copied as it was clicked
    ThreadActionControl m_ActionControl;

    bool m_HasRead = false; // a read was taken in since the selection changed
    Platform::ThreadsReadStatus m_Status = Platform::ThreadsReadStatus::Ok;
    bool m_Truncated = false;
    Detail::ThreadsColumn m_SortColumn = Detail::ThreadsColumn::Cpu;
    bool m_SortAscending = false;
    bool m_FilterDirty = true;
    bool m_WindowsPriorities = false; // the last read's priorities are Windows' (the column says PRIORITY)
    bool m_CanActOnThreads = false;   // the rows offer Suspend, Resume and End (#3069)
    bool m_OpenRowMenu = false;       // the row menu opens on the next render
};

} // namespace App
