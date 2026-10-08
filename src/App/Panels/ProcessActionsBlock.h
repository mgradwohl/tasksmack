#pragma once

// The Overview's Actions block (#1493): the process-control buttons (ProcessActionsView) and the
// priority rows (ProcessPriorityView) under one "Actions" header beside the Identity and Runtime
// blocks, where they had a tab of their own before. One compact, left-aligned stack: the buttons on
// one row at their labels' width, then "Priority [class] [Apply]" (and on Linux the I/O priority row),
// then the result line. This only places the two views; their behaviour -- the confirm dialog, the
// disabled states, the result and error lines, Apply -- is theirs.
//
// Where the block goes is ProcessDetailsLayout::computeActionsBlockLayout()'s decision, from the width
// measure() reports; render() draws it there. Neither creates a probe: the IProcessActions stays owned
// by ProcessDetailsPanel and is passed in each frame.

#include "Platform/IProcessActions.h"
#include "ProcessDetailsLayout.h"

#include <cstdint>
#include <optional>
#include <string>

namespace App
{

class ProcessActionsView;
class ProcessPriorityView;

namespace ProcessActionsBlock
{

/// Whether the block has anything to offer: the platform can run at least one action, or set
/// priority or I/O priority. Without, it is not drawn at all.
[[nodiscard]] constexpr bool hasAnyAction(const Platform::ProcessActionCapabilities& capabilities) noexcept
{
    return capabilities.canTerminate || capabilities.canKill || capabilities.canStop || capabilities.canContinue ||
           capabilities.canSetPriority || capabilities.canSetIoPriority;
}

/// What the block's rows need across, in pixels, at the current font (needs an ImGui frame).
struct Widths
{
    float buttons = 0.0F;  ///< The process-control buttons' row.
    float priority = 0.0F; ///< The widest priority row; 0 when the platform can set neither priority.
    float padding = 0.0F;  ///< The block's own horizontal padding, both sides together.

    /// The block's width: its widest row, padded.
    [[nodiscard]] float content() const noexcept
    {
        return (buttons > priority ? buttons : priority) + padding;
    }
};

/// Measures the widths computeActionsBlockLayout() lays the block out from.
[[nodiscard]] Widths measure(const Platform::ProcessActionCapabilities& capabilities);

/// What render() draws with: the views, and the process they act on this frame.
struct Context
{
    ProcessActionsView* actionsView = nullptr;
    ProcessPriorityView* priorityView = nullptr;
    Platform::IProcessActions* actions = nullptr; ///< May be null: the views then report actions unavailable
    Platform::ProcessActionCapabilities capabilities;
    const std::string* processName = nullptr;
    Platform::ProcessTarget target{.pid = -1, .startTimeTicks = 0};
    std::optional<std::int32_t> currentNice;
};

/// Draws the block at the cursor: the "Actions" header, then a child @p layout.width wide holding the
/// button row, the priority row(s) and the result line, one under the other. Beside the Identity and
/// Runtime blocks (layout.besideInfo) the child is @p rowChildHeight tall, the height of theirs;
/// wrapped below them it takes the height its content needs. Returns that height, padding included,
/// for the next frame's computeActionsBlockLayout(), or 0 when the child was not drawn (scrolled out
/// of view). Text in the block (the result and error lines) wraps at its edge rather than being clipped.
///
/// The confirm dialog is not drawn here, since the child, and all in it, is skipped while scrolled out
/// of view. The caller submits it every frame with ProcessActionsView::renderConfirmation().
[[nodiscard]] float render(const Context& context, const ProcessDetailsLayout::ActionsBlockLayout& layout, float rowChildHeight);

} // namespace ProcessActionsBlock

} // namespace App
