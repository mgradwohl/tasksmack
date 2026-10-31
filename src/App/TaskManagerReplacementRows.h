#pragma once

// Replace Task Manager's ImGui views (#3072): the Settings dialog's rows and the startup notice. Each
// draws a TaskManagerReplacementView and returns what the user asked for; the layers hand that to the
// controller. Split out so the tests draw them headless (tests/App/test_TaskManagerReplacementRender.cpp).

#include "App/TaskManagerReplacementController.h"

namespace App::TaskManagerReplacementRows
{

/// What the Settings rows keep between frames.
struct RowState
{
    /// The toggle was clicked while another program has Task Manager: the confirmation shows.
    bool confirmingReplaceOther = false;
};

/// The Settings rows: the toggle, its note, the state line, the portable copy's warning, a Repair button
/// while it starts another TaskSmack, the confirmation before another program's Debugger is replaced,
/// and the last change's result. Text wraps at @p wrapWidth pixels from the rows' left edge, so a long
/// path doesn't widen the auto-sizing dialog. Draws nothing for an unsupported view.
[[nodiscard]] TaskManagerRequest renderSettings(const TaskManagerReplacementView& view, RowState& state, float wrapWidth);

/// What the startup notice keeps between frames.
struct NoticeState
{
    bool open = false;  ///< Shown: set once, when the first read says so (TaskManagerReplacementText::showsNotice())
    bool acted = false; ///< Repair or Turn off was chosen: the notice shows its result, then Close
};

/// The startup notice, a window of its own at the bottom right that doesn't block anything: the text,
/// then Repair, Turn off and Not now; once one was chosen, its result and Close. Closing it, by Not now,
/// Close or its title bar's button, clears @c open.
[[nodiscard]] TaskManagerRequest renderNotice(const TaskManagerReplacementView& view, NoticeState& state);

} // namespace App::TaskManagerReplacementRows
