#pragma once

// The process-action confirmation dialog, shared by Process Details' Actions tab and the Processes
// table's row menu (#1209), so a Terminate or Kill asks the same question, with the same buttons in
// the same colours, wherever it starts.

#include "ProcessDetailsPanel_ActionHelpers.h"
#include "UI/Widgets.h"

#include <cstdint>
#include <string_view>

namespace App::ProcessActionConfirm
{

/// The theme's danger fills, for the buttons that end a process (Detail::isDestructiveAction(), #1273).
[[nodiscard]] UI::Widgets::ButtonFills dangerButtonFills();

/// What the user did with the dialog this frame.
enum class Outcome : std::uint8_t
{
    None,      ///< Not open, or still open
    Confirmed, ///< The action's button was pressed; the dialog has closed
    Cancelled, ///< Cancel was pressed; the dialog has closed
};

/// Draws the "Kill firefox (PID 1234)?" modal while it is open. Opens it on the frames `showRequested`
/// is true, and clears that flag when the user confirms or cancels. Call it every frame, outside any
/// table, from the window the request came from.
///
/// With `dismiss`, the modal is closed unconfirmed if it is open (returning Cancelled) and
/// `showRequested` is cleared: for a caller whose target changed while the modal was up, since
/// clearing the flag alone does not close a modal ImGui already has open.
Outcome render(bool& showRequested, Detail::ProcessAction action, std::string_view processName, std::int32_t pid, bool dismiss = false);

/// The same modal, same geometry and buttons, with a title and question the caller has already
/// built: the Processes table's batch actions (#804), whose question lists several processes a line
/// each (ProcessBatch::confirmBody()), and its single-process actions, whose text is built once when
/// the action is requested rather than every frame the dialog is up.
Outcome
renderText(bool& showRequested, Detail::ProcessAction action, std::string_view title, std::string_view question, bool dismiss = false);

/// The same modal for a change that is not one of the ProcessActions, its confirm button labelled
/// @p confirmLabel (in the danger fill when @p destructive): the Processes table's batch priority
/// change (#1484), "[Cancel][Set Priority]".
Outcome renderLabelled(bool& showRequested,
                       const char* confirmLabel,
                       bool destructive,
                       std::string_view title,
                       std::string_view question,
                       bool dismiss = false);

} // namespace App::ProcessActionConfirm
