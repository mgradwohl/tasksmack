#pragma once

// Process Details' process-control buttons: Terminate / Kill / Suspend / Resume, the confirm dialog
// and its dispatch, and the result line above them (#1179, slice 3). Drawn in the Overview's Actions
// block (ProcessActionsBlock, #1493), beside the priority control (ProcessPriorityView).
//
// The view owns only its UI state. The IProcessActions it dispatches to stays owned by the panel (the
// composition root's Platform::makeProcessActions() result) and is passed in each frame, with the
// capabilities and the target, so this class never creates a probe.
//
// Everything but render() is defined here, free of ImGui, so the confirm and result state can be
// tested against a mock IProcessActions without an ImGui context (test_ProcessActionsView.cpp).

#include "Platform/IProcessActions.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "UI/IconsFontAwesome6.h"

#include <array>
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
    void renderControls(const Platform::ProcessActionCapabilities& capabilities,
                        const std::string& processName,
                        const Platform::ProcessTarget& target);

    /// Draws the last action's result line, when there is one.
    void renderResultLine() const
    {
        renderResultFeedback();
    }

    /// Width of the row renderControls() draws for @p capabilities, at the current font (needs an
    /// ImGui frame); 0 when the platform can run none of the actions.
    [[nodiscard]] static float buttonsRowWidth(const Platform::ProcessActionCapabilities& capabilities);

    /// Submits the confirm dialog while a confirm is pending, and closes it unconfirmed after a
    /// selection change or when @p liveTarget is no longer the captured process; a confirmed action is
    /// dispatched to @p actions. Kept apart from the buttons so it runs even when they are not drawn:
    /// ProcessDetailsPanel calls it at panel scope, since the Actions block's child is skipped while it
    /// is scrolled out of view, and F9 must still open its Kill confirm then (#1493).
    void renderConfirmation(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget)
    {
        renderConfirmDialog(actions, liveTarget);
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
        m_LastResult = {};
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
    /// a captured action, so the shortcut never replaces it. Only ever opens the dialog: the kill itself
    /// still needs the dialog's own button (dispatchConfirmed()).
    bool requestKillShortcut(const Platform::ProcessActionCapabilities& capabilities,
                             const Platform::ProcessTarget& target,
                             std::string processName)
    {
        if (!Detail::killShortcutAllowed(capabilities, target, confirmPending()))
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
    /// @p actions (null gives an "unavailable" error rather than a dereference), show the result, and
    /// clear the pending confirm, so it runs once. With nothing pending this does nothing at all: no
    /// platform call, and the result line is left as it is.
    void dispatchConfirmed(Platform::IProcessActions* actions)
    {
        if (m_ConfirmAction == Detail::ProcessAction::None)
        {
            return;
        }
        const Platform::ProcessTarget& target = m_ConfirmTarget.target;
        const Platform::ProcessActionResult result = (actions != nullptr)
                                                       ? Detail::dispatchProcessAction(*actions, m_ConfirmAction, target)
                                                       : Platform::ProcessActionResult::error("Process actions unavailable");
        m_LastResult = Detail::formatActionResultMessage(m_ConfirmAction, target.pid, result);
        m_ResultSecondsLeft = Detail::ACTION_RESULT_SECONDS;
        cancelConfirm();
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
    void renderResultFeedback() const;
    void renderConfirmDialog(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget);
    void renderButtons(const Platform::ProcessActionCapabilities& capabilities,
                       const std::string& processName,
                       const Platform::ProcessTarget& target);

    bool m_ShowConfirmDialog = false;
    bool m_DismissPending = false; // A selection change asked to close a dialog ImGui may still have open
    Detail::ProcessAction m_ConfirmAction = Detail::ProcessAction::None;
    ConfirmTarget m_ConfirmTarget;
    Detail::ActionResultMessage m_LastResult;
    float m_ResultSecondsLeft = 0.0F;
};

} // namespace App
