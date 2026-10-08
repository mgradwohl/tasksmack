#pragma once

// The Overview's Actions block (#1493): the process-control buttons (ProcessActionsView) and the
// priority control (ProcessPriorityView), under one "Actions" header beside the Identity and Runtime
// blocks, where they had a tab of their own before. This only places the two views; their behaviour
// -- the confirm dialog, the disabled states, the result and error lines, Apply -- is theirs.
//
// Where the block goes is ProcessDetailsLayout::computeActionsBlockLayout()'s decision, from the
// widths measure() reports; render() draws it there. Neither creates a probe: the IProcessActions
// stays owned by ProcessDetailsPanel and is passed in each frame.

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
/// priority. Without, it is not drawn at all.
[[nodiscard]] constexpr bool hasAnyAction(const Platform::ProcessActionCapabilities& capabilities) noexcept
{
    return capabilities.canTerminate || capabilities.canKill || capabilities.canStop || capabilities.canContinue ||
           capabilities.canSetPriority;
}

/// What the block's parts need across, in pixels, at the current font (needs an ImGui frame).
struct Widths
{
    float controls = 0.0F;  ///< The 2x2 button grid, at the buttons' unclipped width.
    float priority = 0.0F;  ///< The priority control; 0 when @p capabilities cannot set priority.
    float columnGap = 0.0F; ///< Between the two parts when they are side by side.
    float padding = 0.0F;   ///< The block's own horizontal padding, both sides together.
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
/// buttons and the priority control, side by side (the buttons @p widths.controls across) or stacked,
/// as @p layout says. Beside the Identity and Runtime blocks (layout.besideInfo) the child is
/// @p rowChildHeight tall, the height of theirs, and scrolls should its content ever be taller; wrapped
/// below them it takes the height its content needs. Text in the block (the result and error lines)
/// wraps at its edge rather than being clipped.
void render(const Context& context, const Widths& widths, const ProcessDetailsLayout::ActionsBlockLayout& layout, float rowChildHeight);

} // namespace ProcessActionsBlock

} // namespace App
