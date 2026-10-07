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
class ProcessActionsView
{
  public:
    /// Draws the tab: the process's name and PID, the result line, the confirm dialog while it is
    /// open, and the buttons @p capabilities allow. A confirmed action is dispatched to @p actions
    /// (which may be null: the result then says actions are unavailable) for @p target.
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

    /// A different process was selected: close the confirm dialog and drop the result line.
    void onSelectionChanged() noexcept
    {
        m_ShowConfirmDialog = false;
        m_LastResult = {};
    }

    /// A button was pressed: ask to confirm @p action.
    void requestAction(Detail::ProcessAction action) noexcept
    {
        m_ConfirmAction = action;
        m_ShowConfirmDialog = true;
    }

    /// The confirm dialog's action was pressed: run the pending action on @p target through @p actions
    /// (null gives an "unavailable" error rather than a dereference), and show the result.
    void dispatchConfirmed(Platform::IProcessActions* actions, const Platform::ProcessTarget& target)
    {
        const Platform::ProcessActionResult result = (actions != nullptr)
                                                       ? Detail::dispatchProcessAction(*actions, m_ConfirmAction, target)
                                                       : Platform::ProcessActionResult::error("Process actions unavailable");
        m_LastResult = Detail::formatActionResultMessage(m_ConfirmAction, target.pid, result);
        m_ResultSecondsLeft = Detail::ACTION_RESULT_SECONDS;
    }

    /// Whether the confirm dialog is open, or requested to open this frame.
    [[nodiscard]] bool confirmRequested() const noexcept
    {
        return m_ShowConfirmDialog;
    }

    /// The action the confirm dialog asks about (the last one requested).
    [[nodiscard]] Detail::ProcessAction pendingAction() const noexcept
    {
        return m_ConfirmAction;
    }

    /// The result line, empty when there is nothing to show.
    [[nodiscard]] const Detail::ActionResultMessage& lastResult() const noexcept
    {
        return m_LastResult;
    }

  private:
    void renderResultFeedback() const;
    void renderConfirmDialog(Platform::IProcessActions* actions, const std::string& processName, const Platform::ProcessTarget& target);
    void renderButtons(const Platform::ProcessActionCapabilities& capabilities);

    bool m_ShowConfirmDialog = false;
    Detail::ProcessAction m_ConfirmAction = Detail::ProcessAction::None;
    Detail::ActionResultMessage m_LastResult;
    float m_ResultSecondsLeft = 0.0F;
};

} // namespace App
