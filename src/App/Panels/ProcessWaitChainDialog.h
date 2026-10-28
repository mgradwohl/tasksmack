#pragma once

// "Analyze wait chain..." (#3084), from the Processes table's row menu and Process Details' Actions card,
// as Task Manager's: a dialog that reads, on a worker of its own, what each of the process's threads is
// waiting for (IProcessActions::readWaitChains()), says so while it reads, then shows each thread's chain
// as a tree -- the thread, what it waits on, the thread or process that holds that, and on -- with the
// threads that are deadlocked first and in the danger colour. A thread of another process in a chain
// offers "End process...": the shared Kill (or Terminate) confirmation (ProcessActionConfirm), then the
// action on the dialog's own action worker, its result shown in the dialog and the chains read again.
//
// Never on the UI thread: a chain can wait on another process. The IProcessActions stays the panel's (the
// composition root's), passed in when the dialog opens; the workers use it until they finish, so the
// panel declares this dialog after its actions (it is destroyed first, waiting for them), and calls
// shutdown() before it lets them go.
//
// Everything but render() is free of ImGui (tests/App/test_ProcessWaitChainDialog.cpp).

#include "App/Panels/BackgroundTask.h"
#include "App/Panels/ProcessActionRun.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Platform/IProcessActions.h"
#include "Platform/ThreadName.h"
#include "Platform/WaitChain.h"
#include "UI/IconsFontAwesome6.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{

namespace WaitChainDetail
{

/// The row menu's item and the Actions card's button ("###" keeps the menu item's ID fixed).
inline constexpr const char* ANALYZE_WAIT_CHAIN_LABEL = ICON_FA_HOURGLASS_HALF " Analyze wait chain...###AnalyzeWaitChain";
inline constexpr const char* ANALYZE_WAIT_CHAIN_TOOLTIP =
    "Show what each of this process's threads is waiting for, and which threads or processes hold it up: "
    "for a process that has stopped responding";
/// What the dialog shows while it reads.
inline constexpr std::string_view READING_TEXT = "Reading the wait chains...";

/// Whether to offer "Analyze wait chain..." where the row menu acts on @p batchCount selected processes (0:
/// the row it was opened on alone; Process Details passes 0): where the platform can read one, for one process.
[[nodiscard]] constexpr bool offersAnalyzeWaitChain(const Platform::ProcessActionCapabilities& capabilities,
                                                    std::size_t batchCount) noexcept
{
    return capabilities.canAnalyzeWaitChain && batchCount == 0;
}

/// A node's kind as the tree names it: "Mutex", "Critical section", "ALPC call"...; "Unknown (13)" for a
/// type Windows added after this build (@p rawType is its value).
[[nodiscard]] std::string objectTypeLabel(Platform::WaitObjectType type, std::uint32_t rawType);

/// A node's state as the tree names it: "Blocked", "Owned", "Access denied"...; empty when unknown.
[[nodiscard]] constexpr std::string_view nodeStatusLabel(Platform::WaitNodeStatus status) noexcept
{
    switch (status)
    {
    case Platform::WaitNodeStatus::NoAccess:
        return "Access denied";
    case Platform::WaitNodeStatus::Running:
        return "Running";
    case Platform::WaitNodeStatus::Blocked:
        return "Blocked";
    case Platform::WaitNodeStatus::PidOnly:
    case Platform::WaitNodeStatus::PidOnlyRpcss:
        return "Thread not known";
    case Platform::WaitNodeStatus::Owned:
        return "Owned";
    case Platform::WaitNodeStatus::NotOwned:
        return "Not owned";
    case Platform::WaitNodeStatus::Abandoned:
        return "Abandoned";
    case Platform::WaitNodeStatus::Error:
        return "Error";
    case Platform::WaitNodeStatus::Unknown:
        break;
    }
    return {};
}

/// The line shown in place of the chains for a read that didn't succeed; empty for Ok.
[[nodiscard]] constexpr std::string_view readStatusText(Platform::WaitChainReadStatus status) noexcept
{
    switch (status)
    {
    case Platform::WaitChainReadStatus::Ok:
        return {};
    case Platform::WaitChainReadStatus::PermissionDenied:
        return "Access denied: the process runs elevated or as another user. Run TaskSmack as administrator to analyze it.";
    case Platform::WaitChainReadStatus::ProcessExited:
        return "The process has exited.";
    case Platform::WaitChainReadStatus::Unsupported:
        return "Analyzing a wait chain isn't available on this system.";
    case Platform::WaitChainReadStatus::IdentityUnknown:
        return "This process can't be identified, so its wait chain isn't read.";
    case Platform::WaitChainReadStatus::Cancelled:
        return "Stopped before it finished.";
    case Platform::WaitChainReadStatus::Failed:
        break;
    }
    return "The wait chain couldn't be read";
}

/// One node of a chain as the tree shows it, below its thread.
struct NodeRow
{
    std::string text;    ///< "Mutex \"Global\\Lock\" (Owned)", "Thread 42 of notepad.exe (PID 7) (Blocked)"
    std::string tooltip; ///< A thread node's wait time and context switches; empty for an object
    // The process End process would end from this row: a thread of another process than the one analyzed,
    // not TaskSmack's own, with a known start time. PID -1: none offered.
    Platform::ProcessTarget endTarget{.pid = -1, .startTimeTicks = 0};
    std::string endName; ///< That process's name, for the confirmation
};

/// One thread's chain as the tree shows it: a header for the thread, then its nodes, each a level deeper.
struct ThreadRows
{
    std::string header;         ///< "Thread 1234: waiting (Mutex)", "Thread 1234: deadlocked", "Thread 1234: running"
    std::string tooltip;        ///< The thread's own wait time and context switches
    std::vector<NodeRow> nodes; ///< The chain after the thread itself
    std::string note;           ///< Why the chain stops short (cut at Windows' limit), or why it couldn't be read
    bool deadlocked = false;    ///< The chain is a cycle
    bool waiting = false;       ///< The thread waits on something (its chain goes on past it)
    bool failed = false;        ///< The chain couldn't be read (note says why)
};

/// @p node's line in the tree, @p analyzedPid being the process the dialog is for.
[[nodiscard]] std::string nodeText(const Platform::WaitChainNode& node, std::int32_t analyzedPid);

/// @p result's chains as the tree shows them, deadlocked threads first, then those waiting on something,
/// then the rest, each group in the order Windows listed them. End process is offered from a node of
/// another process than @p analyzed and @p ownPid (TaskSmack's) when @p canEnd.
[[nodiscard]] std::vector<ThreadRows>
buildThreadRows(const Platform::WaitChainReadResult& result, const Platform::ProcessTarget& analyzed, std::int32_t ownPid, bool canEnd);

/// The line above the tree: "12 threads: 3 waiting, 2 deadlocked", and how many were left out when the
/// read was cut short.
[[nodiscard]] std::string summaryText(const Platform::WaitChainReadResult& result, std::span<const ThreadRows> threads);

} // namespace WaitChainDetail

class ProcessWaitChainDialog
{
  public:
    ProcessWaitChainDialog() = default;
    ~ProcessWaitChainDialog() = default; // the workers, declared last, stop and wait first
    ProcessWaitChainDialog(const ProcessWaitChainDialog&) = delete;
    ProcessWaitChainDialog& operator=(const ProcessWaitChainDialog&) = delete;
    ProcessWaitChainDialog(ProcessWaitChainDialog&&) = delete;
    ProcessWaitChainDialog& operator=(ProcessWaitChainDialog&&) = delete;

    /// Opens the dialog for @p target, named @p name, on the next render(), and starts reading its chains
    /// through @p actions on the worker (null: the dialog says it can't). A read still running for an
    /// earlier process is asked to stop and dropped. @p ownPid is TaskSmack's, never offered to End process.
    void open(Platform::IProcessActions* actions, const Platform::ProcessTarget& target, std::string name, std::int32_t ownPid);

    /// Each frame, outside any table, from the window the request came from: takes in what the workers
    /// finished (update()), and draws the dialog while it is open.
    void render();

    /// Takes in a finished read or End process, and starts a read asked for while another ran (taken in by
    /// a later call). Never waits, unless @p wait (tests): then it blocks for the work already in flight.
    void update(bool wait = false);

    /// Reads the chains again, as Refresh does.
    void refresh();

    /// Closes the dialog on the next render(), as Close does: a read in flight is asked to stop and dropped.
    void close() noexcept;

    /// Before the IProcessActions it was opened with goes away: stops a read in flight and waits for it,
    /// and for an End process still running.
    void shutdown();

    /// Whether the dialog has been asked for or is open.
    [[nodiscard]] bool isOpen() const noexcept
    {
        return m_Requested;
    }

    /// Whether a read runs on the worker (the dialog shows READING_TEXT, unless it has chains already).
    [[nodiscard]] bool reading() const noexcept
    {
        return m_Read.busy() || m_ReadPending;
    }

    [[nodiscard]] const Platform::ProcessTarget& target() const noexcept
    {
        return m_Target;
    }

    /// The last read taken in, for this opening; none while the first is still running.
    [[nodiscard]] const std::optional<Platform::WaitChainReadResult>& result() const noexcept
    {
        return m_Result;
    }

    /// The last read's chains as the tree shows them.
    [[nodiscard]] std::span<const WaitChainDetail::ThreadRows> threads() const noexcept
    {
        return m_Threads;
    }

    [[nodiscard]] const std::string& summary() const noexcept
    {
        return m_Summary;
    }

    /// "End process..." was pressed on a row: ask to confirm ending @p target, named @p name, with the
    /// platform's Kill (else Terminate). Nothing while an End process runs, or where the platform has neither.
    void requestEnd(const Platform::ProcessTarget& target, std::string name);

    /// The process the pending confirmation is for; PID -1 when none is pending.
    [[nodiscard]] const Platform::ProcessTarget& endTarget() const noexcept
    {
        return m_EndTarget;
    }

    /// The action End process runs: Kill where the platform has it (a process holding others up rarely
    /// answers a request to close), else Terminate; None where it has neither.
    [[nodiscard]] Detail::ProcessAction endAction() const noexcept
    {
        return m_EndAction;
    }

    /// The confirmation's button was pressed: ends the pending target on the action worker. Its result
    /// shows in the dialog, and a success reads the chains again.
    void confirmEnd();

    /// Whether an End process runs on the worker.
    [[nodiscard]] bool ending() const noexcept
    {
        return m_EndRun.busy();
    }

    /// The last End process's result line; empty text when there is none.
    [[nodiscard]] const Detail::ActionResultMessage& endResult() const noexcept
    {
        return m_EndResult;
    }

  private:
    [[nodiscard]] static Platform::WaitChainReadResult failedRead(std::string detail)
    {
        Platform::WaitChainReadResult result;
        result.status = Platform::WaitChainReadStatus::Failed;
        result.detail = std::move(detail);
        return result;
    }

    /// Starts the read for this opening, or, while another is still running, asks it to stop and starts
    /// this one once it has (update()).
    void startRead();
    /// startRead()'s read for the current generation, without starting a new one.
    void launchRead();
    void takeResult(Platform::WaitChainReadResult result);
    /// The tree, in a scrolling child.
    void renderThreads();

    Platform::IProcessActions* m_Actions = nullptr;
    Platform::ProcessTarget m_Target{.pid = -1, .startTimeTicks = 0};
    std::string m_Name;
    std::string m_Title; ///< The dialog's title, built when it opens
    std::int32_t m_OwnPid = 0;
    std::optional<Platform::WaitChainReadResult> m_Result;
    std::vector<WaitChainDetail::ThreadRows> m_Threads;
    std::string m_Summary;
    std::uint64_t m_Generation = 0;     ///< Bumped by each open(), refresh() and close()
    std::uint64_t m_ReadGeneration = 0; ///< m_Generation when the read in flight started

    // End process: the pending confirmation's target, and the action.
    Platform::ProcessTarget m_EndTarget{.pid = -1, .startTimeTicks = 0};
    std::string m_EndName;
    Detail::ProcessAction m_EndAction = Detail::ProcessAction::None;
    Detail::ActionResultMessage m_EndResult;

    ImVec2 m_LastWorkSize;         ///< For UI::Widgets::centerNextDialog()
    bool m_Requested = false;      ///< Asked for and not closed
    bool m_ShowRequested = false;  ///< Open the popup on the next render()
    bool m_CloseRequested = false; ///< Close the popup on the next render()
    bool m_ReadPending = false;    ///< A read is wanted once the one in flight (for an earlier generation) ends
    bool m_ShowEndConfirm = false; ///< The End process confirmation is asked for or open

    // The workers use m_Actions until they finish: last, so they are destroyed (stopping and waiting) first.
    ProcessActionRun m_EndRun;
    BackgroundTask<Platform::WaitChainReadResult> m_Read{Platform::WAIT_CHAIN_READ_THREAD_NAME, &failedRead};
};

} // namespace App
