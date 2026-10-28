#pragma once

// The Overview's Actions and Priority cards (#1493, #1789): the process-control buttons
// (ProcessActionsView) and their result line in the Actions card, and the priority rows
// (ProcessPriorityView) in the Priority card, beside the Identity and Runtime cards where the pane
// has room. They had a tab of their own before (#1493), then shared one Actions card that was too
// tall for the Identity/Runtime row on Windows and so always wrapped below it (#1789). This only
// places the two views; their behaviour -- the confirm dialog, the disabled states, the result and
// error lines, Apply -- is theirs.
//
// Where the cards go is ProcessDetailsLayout::chooseTopRowLayout()'s decision, from the widths
// measure() reports; render() draws them there. Neither creates a probe: the IProcessActions stays
// owned by ProcessDetailsPanel and is passed in each frame.

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "ProcessDetailsLayout.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace App
{

class ProcessActionsView;
class ProcessPriorityView;

namespace ProcessActionsBlock
{

/// Whether there is anything to offer: the platform can run at least one action, set priority, I/O
/// priority, affinity or Efficiency mode, write a dump file, or trace system calls (shown even greyed
/// out). Without, neither card is drawn.
[[nodiscard]] constexpr bool hasAnyAction(const Platform::ProcessActionCapabilities& capabilities) noexcept
{
    return capabilities.canTerminate || capabilities.canKill || capabilities.canStop || capabilities.canContinue ||
           capabilities.canSetPriority || capabilities.canSetIoPriority || capabilities.canSetAffinity || capabilities.canCreateDump ||
           capabilities.canSetEfficiencyMode || capabilities.syscallTrace != Platform::SyscallTraceAvailability::Unsupported;
}

/// What the cards' rows need across, in pixels, at the current font (needs an ImGui frame).
struct Widths
{
    float buttons = 0.0F;  ///< The process-control buttons' row; 0 when the platform has none.
    float priority = 0.0F; ///< The widest priority row; 0 when the platform can set neither priority.
    float padding = 0.0F;  ///< A card's own horizontal padding, both sides together.

    /// The Actions card's width: its button row, padded; 0 when there are no buttons (no card).
    [[nodiscard]] float actionsCard() const noexcept
    {
        return buttons > 0.0F ? buttons + padding : 0.0F;
    }

    /// The Priority card's width: its widest row, padded, in whole pixels up; 0 when there is no
    /// priority (no card). ImGui truncates a window's size, which could leave the card a fraction short
    /// of the row it was measured for, and the class slider then dropped its stop names (#1984).
    [[nodiscard]] float priorityCard() const noexcept
    {
        return priority > 0.0F ? std::ceil(priority + padding) : 0.0F;
    }
};

/// Measures the widths chooseTopRowLayout() lays the cards out from.
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
    const Platform::CpuAffinity* currentAffinity = nullptr; ///< Ticks the affinity dialog's boxes (#1887); null: every CPU
    bool efficiencyMode = false;                            ///< The process is in Efficiency mode: its checkbox is ticked (#3071)
};

/// What each card's content needs down, header and padding included, as render() drew it: for the
/// next frame's computeRowCardHeight(). 0 for a card not drawn (absent, or scrolled out of view).
struct Heights
{
    float actions = 0.0F;
    float priority = 0.0F;
};

/// Draws the cards at the cursor, each a ProcessOverviewCard (#1537) as wide as @p layout says (a
/// card of width 0 is not drawn): Actions -- the "Actions" header, the button row and the result
/// line -- then Priority -- the "Priority" header and the priority row(s) -- to its right
/// (layout.priorityBesideActions) or under it. The priority row has no label of its own unless the
/// I/O priority row is under it to tell it from; otherwise the header is its label and shows the
/// current priority on hover (#1874). Both cards are @p cardHeight tall when it is positive, so they
/// share the edges of the row they are on, with their rows centred down the room under the header
/// (#1874), and as tall as their content when it is 0. Text in
/// them (the result and error lines) wraps at the card's edge rather than being clipped. The caller
/// puts the cursor beside Runtime first (ImGui::SameLine()) when layout.besideInfo.
///
/// The confirm dialog is not drawn here, since a card, and all in it, is skipped while scrolled out
/// of view. The caller submits it every frame with ProcessActionsView::renderConfirmation().
[[nodiscard]] Heights render(const Context& context, const ProcessDetailsLayout::TopRowLayout& layout, float cardHeight);

} // namespace ProcessActionsBlock

} // namespace App
