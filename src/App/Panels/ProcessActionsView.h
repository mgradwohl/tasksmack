#pragma once

// Process Details' process-control buttons: Terminate / Kill / Suspend / Resume, the confirm dialog
// and its dispatch, the "Trace system calls (strace)" button (Linux, #182), "Create dump file..."
// (Windows, #807; ProcessDumpView), and the result line above them (#1179, slice 3). Drawn in the
// Overview's Actions block (ProcessActionsBlock, #1493), beside the priority control (ProcessPriorityView).
//
// The view owns only its UI state. The IProcessActions it dispatches to stays owned by the panel (the
// composition root's Platform::makeProcessActions() result) and is passed in each frame, with the
// capabilities and the target, so this class never creates a probe.
//
// Everything but render() is defined here, free of ImGui, so the confirm and result state can be
// tested against a mock IProcessActions without an ImGui context (test_ProcessActionsView.cpp).

#include "App/Panels/AffinityLayoutRead.h"
#include "App/Panels/ProcessActionRun.h"
#include "App/Panels/ProcessAffinityDialog.h"
#include "App/Panels/ProcessDumpView.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "UI/IconsFontAwesome6.h"

#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace App
{

namespace Detail
{

/// Whether the platform can run @p action, from its ProcessActionCapabilities. A button whose action
/// it cannot run is not drawn; its grid cell is left empty. ProcessAction::None is never available.
[[nodiscard]] constexpr bool isActionAvailable(const Platform::ProcessActionCapabilities& capabilities, ProcessAction action) noexcept
{
    switch (action)
    {
    case ProcessAction::Terminate:
        return capabilities.canTerminate;
    case ProcessAction::Kill:
        return capabilities.canKill;
    case ProcessAction::Stop:
        return capabilities.canStop;
    case ProcessAction::Resume:
        return capabilities.canContinue;
    case ProcessAction::SignalHangup:
    case ProcessAction::SignalInterrupt:
    case ProcessAction::SignalQuit:
    case ProcessAction::SignalUser1:
    case ProcessAction::SignalUser2:
        return capabilities.canSendSignals;
    case ProcessAction::None:
        break;
    }
    return false;
}

/// Whether @p captured (taken when a confirm was requested) and @p live are the same process: the same
/// PID and, when both know it, the same start time, so a reused PID is not taken for it (#973).
[[nodiscard]] constexpr bool isSameProcessTarget(const Platform::ProcessTarget& captured, const Platform::ProcessTarget& live) noexcept
{
    if (captured.pid != live.pid)
    {
        return false;
    }
    return captured.startTimeTicks == 0 || live.startTimeTicks == 0 || captured.startTimeTicks == live.startTimeTicks;
}

/// Whether F9 may ask to confirm Kill on @p target (#170): the platform can kill, there is a target
/// (PID > 0), and no confirm is pending (@p confirmPending: requested, open, or holding a captured action
/// and target), so the shortcut never replaces an action already being confirmed. Shared by the Actions
/// block and the Processes table's row-menu confirm.
[[nodiscard]] constexpr bool killShortcutAllowed(const Platform::ProcessActionCapabilities& capabilities,
                                                 const Platform::ProcessTarget& target,
                                                 bool confirmPending) noexcept
{
    return isActionAvailable(capabilities, ProcessAction::Kill) && target.pid > 0 && !confirmPending;
}

/// One button of the Actions block's button row. The label is also the button's ImGui ID.
struct ActionButtonSpec
{
    ProcessAction action = ProcessAction::None;
    const char* label = "";
    const char* tooltip = "";
};

/// The Actions block's buttons in row order: Terminate and Kill, then Suspend and Resume.
/// Terminate and Kill are drawn in the danger colour (isDestructiveAction(), #1273).
/// "Suspend", not "Pause": the same word as the confirm dialog and the result line (#1203).
inline constexpr std::array<ActionButtonSpec, 4> ACTION_BUTTONS{{
    {.action = ProcessAction::Terminate,
     .label = ICON_FA_XMARK " Terminate",
     .tooltip = "Ask the process to exit: it can save its work first, or refuse"},
    {.action = ProcessAction::Kill, .label = ICON_FA_SKULL " Kill", .tooltip = "Force terminate (cannot be caught or ignored)"},
    {.action = ProcessAction::Stop, .label = ICON_FA_PAUSE " Suspend", .tooltip = "Suspend the process until it is resumed"},
    {.action = ProcessAction::Resume, .label = ICON_FA_PLAY " Resume", .tooltip = "Resume a suspended process"},
}};

/// How long a result line stays up after an action is dispatched, in seconds.
inline constexpr float ACTION_RESULT_SECONDS = 5.0F;

/// The "Trace system calls (strace)" button's label, also its ImGui ID.
inline constexpr const char* SYSCALL_TRACE_LABEL = ICON_FA_TERMINAL " Trace system calls (strace)";
/// The Actions card's Set affinity... button (#1887), opening ProcessAffinityDialog.
inline constexpr const char* SET_AFFINITY_LABEL = ICON_FA_MICROCHIP " Set affinity...";

/// How the trace button is drawn.
enum class SyscallTraceButtonState : std::uint8_t
{
    Hidden,   ///< The platform has no such action (Windows): nothing is drawn.
    Disabled, ///< Drawn greyed out; the tooltip says why.
    Enabled,
};

struct SyscallTraceButton
{
    SyscallTraceButtonState state = SyscallTraceButtonState::Hidden;
    const char* tooltip = "";
};

/// The trace button for @p capabilities (found once, when the platform's actions were made) and the
/// selected @p target: hidden where the platform has no tracer at all, disabled with the reason when
/// strace or a terminal is missing or nothing is selected, otherwise enabled. It asks for no confirm:
/// tracing only watches the process (pausing it just for the instant strace attaches).
[[nodiscard]] constexpr SyscallTraceButton syscallTraceButton(const Platform::ProcessActionCapabilities& capabilities,
                                                              const Platform::ProcessTarget& target) noexcept
{
    switch (capabilities.syscallTrace)
    {
    case Platform::SyscallTraceAvailability::Unsupported:
        return {.state = SyscallTraceButtonState::Hidden, .tooltip = ""};
    case Platform::SyscallTraceAvailability::NoTracer:
    case Platform::SyscallTraceAvailability::NoTerminal:
        return {.state = SyscallTraceButtonState::Disabled, .tooltip = Platform::syscallTraceUnavailableReason(capabilities.syscallTrace)};
    case Platform::SyscallTraceAvailability::Available:
        break;
    }
    if (target.pid <= 0)
    {
        return {.state = SyscallTraceButtonState::Disabled, .tooltip = "Select a process to trace"};
    }
    return {.state = SyscallTraceButtonState::Enabled,
            .tooltip = "Open a terminal running strace attached to this process, showing each system call it makes. "
                       "Close the window or press Ctrl+C there to stop tracing; the process keeps running."};
}

/// "Opened strace for PID 321 in a new terminal" / "Could not trace PID 321: ...".
[[nodiscard]] inline ActionResultMessage formatSyscallTraceResult(std::int32_t pid, const Platform::ProcessActionResult& result)
{
    if (result.success)
    {
        return {.ok = true, .text = std::format("Opened strace for PID {} in a new terminal", pid)};
    }
    return {.ok = false, .text = std::format("Could not trace PID {}: {}", pid, result.errorMessage)};
}

} // namespace Detail

/// The Actions block's buttons, confirm dialog and result line for the process Process Details shows.
///
/// The process a confirm asks about is captured when its button is pressed: the dialog names that
/// process and the confirm acts only on it, never on whatever is selected by then. A selection
/// change, or a target that no longer matches the capture, closes the dialog unconfirmed.
class ProcessActionsView
{
  public:
    /// The process a pending confirm is about, as it was when its button was pressed.
    struct ConfirmTarget
    {
        Platform::ProcessTarget target{.pid = -1, .startTimeTicks = 0};
        std::string processName;
    };

    /// Draws the result line, the confirm dialog while it is open, and the buttons @p capabilities
    /// allow: renderControls() and renderConfirmation() together, for a caller that draws both in one
    /// place. A button captures @p target and @p processName for its confirm; a confirmed action is
    /// dispatched to @p actions (which may be null: the result then says actions are unavailable) for
    /// that captured target.
    void render(Platform::IProcessActions* actions,
                const Platform::ProcessActionCapabilities& capabilities,
                const std::string& processName,
                const Platform::ProcessTarget& target);

    /// Draws the buttons @p capabilities allow, on one row at their labels' width, without the
    /// result line or the confirm dialog. The caller draws the header and names the process (the
    /// Overview's Actions block, #1493), draws renderResultLine() under its last row, and calls
    /// renderConfirmation() every frame from a scope that always runs.
    ///
    /// The row ends with "Trace system calls (strace)" where the platform has it (#182), at its own
    /// label's width; like any button that does not fit, it starts a new row. It has no confirm, so a
    /// press launches through @p actions at once (null: the result says actions are unavailable).
    ///
    /// Then "Set affinity..." where the platform can (#1887), placed the same way; it opens the affinity
    /// dialog with @p currentAffinity's CPUs ticked (null: every CPU), for the process captured now.
    ///
    /// Then "Create dump file..." where the platform can write one (#807), placed the same way; it asks
    /// to confirm a dump of the process captured now, into the temp folder.
    void renderControls(Platform::IProcessActions* actions,
                        const Platform::ProcessActionCapabilities& capabilities,
                        const std::string& processName,
                        const Platform::ProcessTarget& target,
                        const Platform::CpuAffinity* currentAffinity = nullptr);

    /// Draws the last action's result line, when there is one, with "Authenticate and retry" beside a
    /// permission refusal the platform can retry as an administrator (#2308).
    void renderResultLine()
    {
        renderResultFeedback();
    }

    /// Whether the result line offers "Authenticate and retry" (#2308).
    [[nodiscard]] bool offersAuthenticateAndRetry() const noexcept
    {
        return m_Retry.has_value() && !m_ActionRun.busy();
    }

    /// Presses "Authenticate and retry", for tests.
    void retryAuthorizedForTesting()
    {
        retryAuthorized();
    }

    /// Width the buttons renderControls() draws for @p capabilities need, at the current font (needs
    /// an ImGui frame): the process-control row, or the trace button alone when that is wider -- the
    /// trace button starts a row of its own whenever it does not fit after them, so the block never
    /// needs both on one line. 0 when the platform has none of the buttons.
    [[nodiscard]] static float buttonsRowWidth(const Platform::ProcessActionCapabilities& capabilities);

    /// Width of the trace button for @p capabilities at the current font (needs an ImGui frame), or 0
    /// where it is hidden (Detail::SyscallTraceButtonState::Hidden).
    [[nodiscard]] static float syscallTraceButtonWidth(const Platform::ProcessActionCapabilities& capabilities);

    /// Width of the Set affinity... button for @p capabilities at the current font, or 0 where the
    /// platform can't set affinity (#1887).
    [[nodiscard]] static float setAffinityButtonWidth(const Platform::ProcessActionCapabilities& capabilities);

    /// Width of the Create dump file... button for @p capabilities at the current font, or 0 where the
    /// platform can't write a dump (#807).
    [[nodiscard]] static float createDumpButtonWidth(const Platform::ProcessActionCapabilities& capabilities);

    /// Submits the confirm dialog while a confirm is pending, and closes it unconfirmed after a
    /// selection change or when @p liveTarget is no longer the captured process; a confirmed action is
    /// dispatched to @p actions. Kept apart from the buttons so it runs even when they are not drawn:
    /// ProcessDetailsPanel calls it at panel scope, since the Actions block's child is skipped while it
    /// is scrolled out of view, and F9 must still open its Kill confirm then (#1493).
    /// The affinity dialog (#1887) runs here too, and is closed unapplied when @p liveTarget is no
    /// longer the process it was opened for; and the dump's confirm, progress and result (#807), its
    /// confirm closed unconfirmed the same way.
    void renderConfirmation(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget)
    {
        m_ActionRun.update(); // a confirmed action, trace launch or affinity change that has finished (#2305)
        renderConfirmDialog(actions, liveTarget);
        renderAffinityDialog(actions, liveTarget);
        m_DumpView.render(actions, &liveTarget);
    }

    /// The dump's state (#807), for tests.
    [[nodiscard]] ProcessDumpView& dumpView() noexcept
    {
        return m_DumpView;
    }

    /// The affinity dialog, for tests: open after Set affinity... is pressed.
    [[nodiscard]] ProcessAffinityDialog& affinityDialog() noexcept
    {
        return m_AffinityDialog;
    }

    /// The update path (#3182): starts the one read of @p actions' CPU layout on its worker, early, so
    /// Set affinity... normally finds it read (null @p actions: none), and hands a layout that has
    /// arrived to a dialog opened before it did. @p actions must outlive this view.
    void updateAffinityLayout(const Platform::IProcessActions* actions)
    {
        m_AffinityLayout.start(actions);
        takeAffinityLayout(false);
    }

    /// Waits for the CPU layout's read, if one was started, and hands the layout to a dialog awaiting it,
    /// as a later frame would: for tests, which press Set affinity... and then tick its boxes.
    void waitForAffinityLayoutForTesting()
    {
        takeAffinityLayout(true);
    }

    /// Advances the result line's timeout by @p deltaSeconds, clearing the line once it runs out.
    void tick(float deltaSeconds) noexcept
    {
        if (m_ResultSecondsLeft > 0.0F)
        {
            m_ResultSecondsLeft -= deltaSeconds;
            if (m_ResultSecondsLeft <= 0.0F)
            {
                m_LastResult = {};
                m_Retry.reset();
            }
        }
    }

    /// A different process was selected: drop the pending confirm and its captured target, have the
    /// next render() close the dialog if ImGui still has it open, and drop the result line. A dismissal
    /// is queued only when a confirm was pending or open: queued with nothing to close, it would wait
    /// for the next render() and close the next confirm requested before it, such as F9's (#170).
    void onSelectionChanged() noexcept
    {
        m_DismissPending = m_DismissPending || confirmPending();
        cancelConfirm();
        m_AffinityDialog.dismiss();
        m_DumpView.onSelectionChanged(); // its confirm only: a dump already running goes on
        m_LastResult = {};
        m_Retry.reset(); // never offer to retry an action on a process no longer shown
        ++m_Generation;  // an action still running reports to nobody: its process is no longer shown (#2389)
    }

    /// Whether a confirm is requested, open, or holds a captured action and target.
    [[nodiscard]] bool confirmPending() const noexcept
    {
        return m_ShowConfirmDialog || m_ConfirmAction != Detail::ProcessAction::None;
    }

    /// The dialog closed without acting (Cancel, or dismissed): nothing is pending any more, so
    /// pendingAction() is None, confirmTarget() has PID -1, and dispatchConfirmed() does nothing.
    void cancelConfirm() noexcept
    {
        m_ShowConfirmDialog = false;
        m_ConfirmAction = Detail::ProcessAction::None;
        m_ConfirmTarget = {};
    }

    /// A button was pressed: ask to confirm @p action on @p target, named @p processName.
    void requestAction(Detail::ProcessAction action, const Platform::ProcessTarget& target, std::string processName)
    {
        m_ConfirmAction = action;
        m_ConfirmTarget = {.target = target, .processName = std::move(processName)};
        m_ShowConfirmDialog = true;
    }

    /// F9 (#170): ask to confirm Kill on @p target, as the Kill button does, capturing it now. Refused
    /// (returns false, nothing changes) when Detail::killShortcutAllowed() says no: the platform cannot
    /// kill, there is no target (PID <= 0), or another confirm is pending -- requested, open, or holding
    /// a captured action, so the shortcut never replaces it -- or a dump's dialog is up (#807). Only ever
    /// opens the dialog: the kill itself still needs the dialog's own button (dispatchConfirmed()).
    bool requestKillShortcut(const Platform::ProcessActionCapabilities& capabilities,
                             const Platform::ProcessTarget& target,
                             std::string processName)
    {
        // Not while an action runs either: the Kill button is disabled then, and a confirmed kill would only
        // be refused with "Another action is still running" (#2390)
        if (!Detail::killShortcutAllowed(capabilities, target, confirmPending() || m_DumpView.active() || m_ActionRun.busy()))
        {
            return false;
        }
        // Nothing is pending, so a dismissal still queued has no dialog of its own to close; left
        // queued, the next render() would close this confirm as soon as it opens.
        m_DismissPending = false;
        requestAction(Detail::ProcessAction::Kill, target, std::move(processName));
        return true;
    }

    /// Whether the dialog must be closed unconfirmed this frame, given the selection's @p liveTarget:
    /// after a selection change (once; the request is consumed), or while a confirm is pending for a
    /// different process than the one now selected.
    [[nodiscard]] bool takeDismiss(const Platform::ProcessTarget& liveTarget) noexcept
    {
        const bool selectionChanged = std::exchange(m_DismissPending, false);
        const bool targetMoved = m_ShowConfirmDialog && !Detail::isSameProcessTarget(m_ConfirmTarget.target, liveTarget);
        if (targetMoved)
        {
            cancelConfirm();
        }
        return selectionChanged || targetMoved;
    }

    /// The confirm dialog's action was pressed: run the pending action on the captured target through
    /// @p actions (null gives an "unavailable" error rather than a dereference) on the action worker
    /// (#2305), and clear the pending confirm, so it runs once; the result line shows its progress, then
    /// its result. With nothing pending this does nothing at all: no platform call, and the result line
    /// is left as it is.
    void dispatchConfirmed(Platform::IProcessActions* actions)
    {
        if (m_ConfirmAction == Detail::ProcessAction::None)
        {
            return;
        }
        const Detail::ProcessAction action = m_ConfirmAction;
        const Platform::ProcessTarget target = m_ConfirmTarget.target;
        cancelConfirm();
        runAction(
            actions,
            std::format("{} PID {}...", Detail::actionProgressLabel(action), target.pid),
            [actions, action, target] { return Detail::dispatchProcessAction(*actions, action, target); },
            [action, pid = target.pid](const Platform::ProcessActionResult& result)
            { return Detail::formatActionResultMessage(action, pid, result); },
            Detail::authorizedActionFor(action, target));
    }

    /// The trace button was pressed: open strace on @p target through @p actions (null gives an
    /// "unavailable" error) on the action worker, and show the outcome in the result line. The worker
    /// returns once the terminal has started; it never waits for the trace.
    void launchSyscallTrace(Platform::IProcessActions* actions, const Platform::ProcessTarget& target)
    {
        runAction(
            actions,
            std::format("Starting a trace of PID {}...", target.pid),
            [actions, target] { return actions->launchSyscallTrace(target); },
            [pid = target.pid](const Platform::ProcessActionResult& result) { return Detail::formatSyscallTraceResult(pid, result); },
            std::nullopt); // a trace runs as the user, in a terminal: not one the helper does
    }

    /// Whether an action runs on the worker now (#2305): the action buttons are disabled meanwhile.
    [[nodiscard]] bool actionRunning() const noexcept
    {
        return m_ActionRun.busy();
    }

    /// Waits for the running action and takes its result in, as the next frame would: for tests, which
    /// read the result line straight after dispatching.
    void waitForActionForTesting()
    {
        m_ActionRun.update(/*wait=*/true);
    }

    /// Whether the confirm dialog is open, or requested to open this frame.
    [[nodiscard]] bool confirmRequested() const noexcept
    {
        return m_ShowConfirmDialog;
    }

    /// The action the confirm dialog asks about (None when nothing is pending).
    [[nodiscard]] Detail::ProcessAction pendingAction() const noexcept
    {
        return m_ConfirmAction;
    }

    /// The process the pending confirm is about (PID -1 when nothing is pending).
    [[nodiscard]] const ConfirmTarget& confirmTarget() const noexcept
    {
        return m_ConfirmTarget;
    }

    /// The result line, empty when there is nothing to show.
    [[nodiscard]] const Detail::ActionResultMessage& lastResult() const noexcept
    {
        return m_LastResult;
    }

  private:
    /// Turns an action's result into the result line.
    using ResultFormat = std::function<Detail::ActionResultMessage(const Platform::ProcessActionResult&)>;

    /// Runs @p work (a platform call through @p actions) on the action worker, showing @p pending until it
    /// finishes; @p format turns its result into the result line. Null @p actions is the "unavailable"
    /// result at once, without a worker. A permission refusal of @p retry, where the platform can ask
    /// for authorization, offers "Authenticate and retry" beside the result line (#2308).
    template<typename Work>
    void runAction(Platform::IProcessActions* actions,
                   std::string pending,
                   Work work,
                   ResultFormat format,
                   std::optional<Platform::AuthorizedAction> retry)
    {
        if (actions == nullptr)
        {
            showResult(format(Platform::ProcessActionResult::error("Process actions unavailable")));
            return;
        }
        if (m_ActionRun.busy())
        {
            showResult(format(Platform::ProcessActionResult::error("Another action is still running")));
            return;
        }
        m_RunGeneration = m_Generation;
        static_cast<void>(m_ActionRun.start(
            std::move(pending),
            std::move(work),
            // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks) - the std::function moved into the finish is owned by it, not leaked
            [this, actions, generation = m_Generation, format = std::move(format), retry = std::move(retry)](
                const Platform::ProcessActionResult& result)
            {
                if (generation != m_Generation)
                {
                    return; // the selection changed while it ran: not this process's result to show (#2389)
                }
                showResult(format(result));
                if (result.permissionDenied && retry.has_value() && actions->actionCapabilities().canAuthorizeInteractively)
                {
                    m_Retry = Retry{.action = *retry, .actions = actions, .format = format};
                    m_ResultSecondsLeft = Detail::AUTHORIZE_RESULT_SECONDS;
                }
            }));
    }

    /// The result line's "Authenticate and retry" was pressed: the refused action again, as an
    /// administrator, after the password prompt (IProcessActions::runAuthorized(), #2308).
    void retryAuthorized()
    {
        if (!m_Retry.has_value() || m_ActionRun.busy())
        {
            return;
        }
        Retry retry = std::move(*m_Retry);
        m_Retry.reset();
        runAction(
            retry.actions,
            std::format("Waiting for authorization to act on PID {}...", retry.action.target.pid),
            [actions = retry.actions, action = retry.action](const std::stop_token& stopToken)
            { return actions->runAuthorized(action, stopToken); },
            std::move(retry.format),
            std::nullopt); // a refusal to the administrator isn't offered again
    }

    /// Takes in the CPU layout's read once it has finished, and hands the layout to an affinity dialog
    /// awaiting it. Never waits, unless @p wait (tests).
    void takeAffinityLayout(bool wait)
    {
        const Platform::CpuAffinityLayout* layout = m_AffinityLayout.layout(wait);
        if (layout != nullptr && m_AffinityDialog.awaitingLayout())
        {
            m_AffinityDialog.setLayout(*layout);
        }
    }

    void showResult(Detail::ActionResultMessage message) noexcept
    {
        m_LastResult = std::move(message);
        m_ResultSecondsLeft = Detail::ACTION_RESULT_SECONDS;
        m_Retry.reset();
    }

    void renderResultFeedback();
    void renderConfirmDialog(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget);
    /// Returns whether any button was drawn, so the trace button knows whether to follow on the row.
    bool renderButtons(const Platform::ProcessActionCapabilities& capabilities,
                       const std::string& processName,
                       const Platform::ProcessTarget& target);
    /// Set affinity... (#1887): at the row's end at its own width, as the trace button is. A press opens
    /// the dialog with @p actions' CPU layout (null: none) to group its boxes by (#1881), as read once on
    /// its worker; before that has arrived, the dialog opens awaiting it (#3182).
    void renderSetAffinityButton(const Platform::IProcessActions* actions,
                                 const Platform::ProcessActionCapabilities& capabilities,
                                 const std::string& processName,
                                 const Platform::ProcessTarget& target,
                                 const Platform::CpuAffinity* currentAffinity,
                                 bool afterButtons);
    /// The affinity dialog while it is open; closed unapplied once @p liveTarget moves on. Apply sets
    /// the CPUs through @p actions (null: unavailable) and shows the result line.
    void renderAffinityDialog(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget);
    void renderSyscallTraceButton(Platform::IProcessActions* actions,
                                  const Platform::ProcessActionCapabilities& capabilities,
                                  const Platform::ProcessTarget& target,
                                  bool afterButtons);
    /// Create dump file... (#807): at the row's end at its own width, as the trace button is. A press
    /// asks to confirm a dump of @p target into the remembered folder (else the temp folder), its free
    /// space read through @p actions (#2001).
    void renderCreateDumpButton(Platform::IProcessActions* actions,
                                const Platform::ProcessActionCapabilities& capabilities,
                                const std::string& processName,
                                const Platform::ProcessTarget& target,
                                bool afterButtons);

    bool m_ShowConfirmDialog = false;
    bool m_DismissPending = false; // A selection change asked to close a dialog ImGui may still have open
    Detail::ProcessAction m_ConfirmAction = Detail::ProcessAction::None;
    ConfirmTarget m_ConfirmTarget;
    Detail::ActionResultMessage m_LastResult;
    /// A permission refusal "Authenticate and retry" would redo (#2308): the action, the actions it was
    /// refused through (the panel's, which outlive this view), and its result line's wording.
    struct Retry
    {
        Platform::AuthorizedAction action;
        Platform::IProcessActions* actions = nullptr;
        ResultFormat format;
    };
    std::optional<Retry> m_Retry;
    std::uint64_t m_Generation = 0;         // bumped by onSelectionChanged() (#2389)
    std::uint64_t m_RunGeneration = 0;      // m_Generation when the running action started
    ProcessAffinityDialog m_AffinityDialog; // Set affinity... (#1887)
    // The dialog's CPU layout, read once on its worker (#3182). Its worker uses the panel's IProcessActions
    // until it finishes: the panel declares this view after its actions, so its destructor's wait goes first.
    AffinityLayoutRead m_AffinityLayout;
    // Create dump file... (#807). Its worker uses the panel's IProcessActions until it finishes: the
    // panel declares this view after its actions, so the view (and its destructor's wait) goes first.
    ProcessDumpView m_DumpView;
    float m_ResultSecondsLeft = 0.0F;
    // The confirmed action, trace launch or affinity change in flight (#2305). Its worker calls the
    // panel's IProcessActions until it finishes: last, so it is destroyed (and waited for) first, and the
    // panel declares this view after its actions.
    ProcessActionRun m_ActionRun;
};

} // namespace App
