#pragma once

// Process Details' Actions tab: the Terminate / Kill / Suspend / Resume buttons, the confirm dialog
// and its dispatch, and the result line under them (#1179, slice 3). The priority control below the
// buttons stays in ProcessDetailsPanel for now.
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
#include <cstddef>
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

/// One button of the Actions tab's 2x2 grid. The label is also the button's ImGui ID.
struct ActionButtonSpec
{
    ProcessAction action = ProcessAction::None;
    const char* label = "";
    const char* tooltip = "";
};

/// The Actions tab's buttons in grid order, row by row: Terminate and Kill, then Suspend and Resume.
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

/// Buttons per grid row.
inline constexpr std::size_t ACTION_BUTTON_GRID_COLUMNS = 2;

/// How long a result line stays up after an action is dispatched, in seconds.
inline constexpr float ACTION_RESULT_SECONDS = 5.0F;

} // namespace Detail

/// The Actions tab's buttons, confirm dialog and result line for the process Process Details shows.
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

    /// Draws the tab: the process's name and PID, the result line, the confirm dialog while it is
    /// open, and the buttons @p capabilities allow. A button captures @p target and @p processName for
    /// its confirm; a confirmed action is dispatched to @p actions (which may be null: the result then
    /// says actions are unavailable) for that captured target.
    void render(Platform::IProcessActions* actions,
                const Platform::ProcessActionCapabilities& capabilities,
                const std::string& processName,
                const Platform::ProcessTarget& target);

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
    /// next render() close the dialog if ImGui still has it open, and drop the result line.
    void onSelectionChanged() noexcept
    {
        cancelConfirm();
        m_DismissPending = true;
        m_LastResult = {};
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
