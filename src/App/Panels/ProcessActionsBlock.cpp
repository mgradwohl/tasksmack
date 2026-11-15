#include "ProcessActionsBlock.h"

#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"
#include "ProcessActionsView.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "ProcessIoPriorityView.h"
#include "ProcessOverviewCard.h"
#include "ProcessPriorityView.h"
#include "UI/IconsFontAwesome6.h"

#ifdef _WIN32
#include "ProcessDetailsPanel_PriorityHelpers.h" // WINDOWS_PRIORITY_SLIDER for the Windows class slider (#1538)
#endif

#include <imgui.h>

#include <algorithm>
#include <string>

namespace App::ProcessActionsBlock
{

namespace
{

/// An Apply button as ProcessPriorityView and ProcessIoPriorityView size theirs: its label's width,
/// the same rule as Terminate's and Kill's (#1537).
[[nodiscard]] float applyButtonWidth(const char* label)
{
    return ProcessDetailsLayout::computeActionButtonWidth(ImGui::CalcTextSize(label).x, ImGui::GetStyle().FramePadding.x);
}

/// The widest row of the Priority and I/O priority cards, which share a column and its width (#3741): each
/// card's slider and its buttons under it.
[[nodiscard]] float priorityRowsWidth(const Platform::ProcessActionCapabilities& capabilities)
{
    const float emPx = ImGui::GetFontSize();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float width = 0.0F;
    if (capabilities.canSetPriority)
    {
#ifdef _WIN32
        // The class slider at the narrowest that shows all five stop names (#1538), and Apply under it.
        // Runtime's Priority row shows the current class, not this row (#1537).
        width =
            std::max(Detail::discretePrioritySliderMinWidth(Detail::WINDOWS_PRIORITY_SLIDER), applyButtonWidth(ICON_FA_CHECK "  Apply"));
#else
        // The gradient slider row, which fits itself to the width it is given.
        width = ProcessDetailsLayout::PRIORITY_SLIDER_ROW_WIDTH_EM * emPx;
#endif
    }
    if (capabilities.canSetIoPriority)
    {
        // The I/O slider at the nice slider's row width (it fits itself to the width it is given, as that
        // one does), and [Reset to default] [Apply] under it (#1540).
        width = std::max({
            width,
            ProcessDetailsLayout::PRIORITY_SLIDER_ROW_WIDTH_EM * emPx,
            applyButtonWidth("Reset to default") + spacing + applyButtonWidth(ICON_FA_CHECK "  Apply"),
        });
    }
    return width;
}

/// Draws one of the cards render() draws, @p width wide and exactly as tall as its content (#3741): the
/// header (@p icon, @p title) and @p rows, with text in them (result and error lines) wrapped at the card's
/// edge instead of running out of it. The card grows while a result or error line shows and shrinks back
/// after. It used to be given the tallest card's height on its row (#1789) and centre its rows in it
/// (#1874), which left blank card above and below them.
template<typename Rows> void renderCard(const char* id, const char* icon, const char* title, float width, const Rows& rows)
{
    (void) ProcessOverviewCard::render(id,
                                       icon,
                                       title,
                                       ImVec2(width, 0.0F),
                                       ImGuiChildFlags_AutoResizeY,
                                       [&]
                                       {
                                           ImGui::PushTextWrapPos(0.0F);
                                           rows();
                                           ImGui::PopTextWrapPos();
                                       });
}

/// Why the slider's Apply waits while the Actions card's Set Realtime priority or Efficiency mode change
/// is asked about or runs (#3429, #3467).
constexpr const char* ACTIONS_CARD_HOLD_REASON = "Waiting for Set Realtime priority or Efficiency mode to finish";

/// Set Realtime... under the priority slider (#3429, Windows): Realtime is not a stop on the slider, since
/// it is never applied without the warning, so the button asks the Actions card's confirm, whose result
/// line (and Retry as administrator) says how it went. Nothing where the platform can't set it.
void renderSetRealtimeButton(const Context& context)
{
    const bool isRealtime = context.currentNice.has_value() &&
                            Domain::Priority::classFromNice(*context.currentNice) == Domain::Priority::PriorityClass::Realtime;
    // Held while the slider's Apply (or its Retry as administrator) runs: both set this process's priority,
    // and the older could finish last and overwrite Realtime (#3429). So while a change for it runs in the
    // Processes table (#3467).
    const Detail::RealtimePriorityItem item = Detail::holdRealtimePriorityItem(
        Detail::realtimePriorityItem(context.capabilities,
                                     context.target,
                                     isRealtime,
                                     context.actionsView->actionRunning() || context.actionsView->confirmPending(),
                                     0,
                                     context.priorityView->applying()),
        context.priorityHeldElsewhere);
    if (item.state == Detail::RealtimePriorityState::Hidden)
    {
        return;
    }
    ImGui::Spacing();
    const bool enabled = item.state == Detail::RealtimePriorityState::Enabled;
    if (!enabled)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton(Detail::SET_REALTIME_PRIORITY_BUTTON_LABEL))
    {
        context.actionsView->requestAction(Detail::ProcessAction::SetRealtimePriority, context.target, *context.processName);
    }
    if (!enabled)
    {
        ImGui::EndDisabled();
    }
    ImGui::SetItemTooltip("%s", item.tooltip); // a disabled button still says why
}

} // namespace

const char* actionsHoldReason(bool sliderApplying, const char* priorityHeldElsewhere) noexcept
{
    return sliderApplying ? "Waiting for the priority change being applied to finish" : priorityHeldElsewhere;
}

Widths measure(const Platform::ProcessActionCapabilities& capabilities)
{
    return Widths{
        .buttons = ProcessActionsView::buttonsRowWidth(capabilities),
        .priority = priorityRowsWidth(capabilities),
        .padding = ImGui::GetStyle().WindowPadding.x * 2.0F,
    };
}

void render(const Context& context, const ProcessDetailsLayout::TopRowLayout& layout)
{
    if (context.actionsView == nullptr || context.priorityView == nullptr || context.processName == nullptr)
    {
        return;
    }

    // The Actions card's priority changes -- Efficiency mode, and a refused Set Realtime priority's or
    // Efficiency mode's "Retry as administrator" -- wait for the slider's apply (#3429), and for a change for
    // this process in the Processes table (#3467).
    context.actionsView->holdPriorityChanges(actionsHoldReason(context.priorityView->applying(), context.priorityHeldElsewhere));

    const bool drawActions = layout.actionsWidth > 0.0F;
    if (drawActions)
    {
        renderCard("ProcessActionsBlock",
                   ICON_FA_GEARS,
                   "Actions",
                   layout.actionsWidth,
                   [&]
                   {
                       context.actionsView->renderControls(context.actions,
                                                           context.capabilities,
                                                           *context.processName,
                                                           context.target,
                                                           context.currentAffinity,
                                                           context.efficiencyMode,
                                                           context.currentSchedPolicy,
                                                           context.currentRtPriority);
                       context.actionsView->renderResultLine();
                   });
    }

    if (layout.priorityWidth <= 0.0F)
    {
        return;
    }
    if (drawActions && layout.priorityBesideActions)
    {
        ImGui::SameLine();
    }
    // Priority, then I/O priority under it: one column, the two cards its width (#3741).
    ImGui::BeginGroup();
    if (context.capabilities.canSetPriority)
    {
        renderCard("ProcessPriorityBlock",
                   ICON_FA_GAUGE_HIGH,
                   "Priority",
                   layout.priorityWidth,
                   [&]
                   {
                       // The header is the control's label, and so shows the current priority on hover (#1874).
                       ProcessPriorityView::setCurrentPriorityTooltip(context.currentNice);
                       // Apply waits while Set Realtime priority or Efficiency mode is asked about or runs
                       // (#3429, #3467), or a change for this process runs in the Processes table (#3467).
                       context.priorityView->holdForOtherPriorityChange(
                           context.actionsView->priorityChangeInFlight() ? ACTIONS_CARD_HOLD_REASON : context.priorityHeldElsewhere);
                       context.priorityView->render(context.actions, context.capabilities, context.currentNice, context.target);
                       renderSetRealtimeButton(context);
                   });
    }
    if (context.capabilities.canSetIoPriority)
    {
        renderCard("ProcessIoPriorityBlock",
                   ICON_FA_HARD_DRIVE,
                   "I/O priority",
                   layout.priorityWidth,
                   [&]
                   {
                       // The header is the control's label: the current I/O priority on hover.
                       // Built only while hovered: the text is an allocation, and the card draws every frame (#3806).
                       if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                       {
                           const std::string current =
                               context.priorityView->ioPriorityView().currentText(context.currentNice, context.currentSchedPolicy);
                           ImGui::SetTooltip("%s", current.c_str());
                       }
                       context.priorityView->renderIoPriority(
                           context.actions, context.capabilities, context.currentNice, context.target, context.currentSchedPolicy);
                   });
    }
    ImGui::EndGroup();
}

} // namespace App::ProcessActionsBlock
