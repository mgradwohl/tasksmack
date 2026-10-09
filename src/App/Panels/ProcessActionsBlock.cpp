#include "ProcessActionsBlock.h"

#include "Platform/IProcessActions.h"
#include "ProcessActionsView.h"
#include "ProcessDetailsLayout.h"
#include "ProcessOverviewCard.h"
#include "ProcessPriorityView.h"
#include "UI/IconsFontAwesome6.h"

#ifdef _WIN32
#include "ProcessDetailsPanel_PriorityHelpers.h" // WINDOWS_PRIORITY_SLIDER for the Windows class slider (#1538)
#endif

#include <imgui.h>

#include <algorithm>

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

/// The priority rows ProcessPriorityView draws for @p capabilities: label, controls and Apply.
[[nodiscard]] float priorityRowsWidth(const Platform::ProcessActionCapabilities& capabilities)
{
    const float emPx = ImGui::GetFontSize();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float width = 0.0F;
    if (capabilities.canSetPriority)
    {
#ifdef _WIN32
        // Priority, over the class slider at the narrowest that shows all five stop names (#1538), and
        // Apply under it. Runtime's Priority row shows the current class, not this row (#1537).
        width = std::max({ImGui::CalcTextSize("Priority").x,
                          Detail::discretePrioritySliderMinWidth(Detail::WINDOWS_PRIORITY_SLIDER),
                          applyButtonWidth(ICON_FA_CHECK "  Apply")});
#else
        // The gradient slider row, which fits itself to the width it is given.
        width = ProcessDetailsLayout::PRIORITY_SLIDER_ROW_WIDTH_EM * emPx;
#endif
    }
    if (capabilities.canSetIoPriority)
    {
        // "I/O priority", its slider under it at the nice slider's row width (it fits itself to the width
        // it is given, as that one does), and [Reset to default] [Apply] under the slider (#1540).
        width = std::max({width,
                          ImGui::CalcTextSize("I/O priority").x,
                          ProcessDetailsLayout::PRIORITY_SLIDER_ROW_WIDTH_EM * emPx,
                          applyButtonWidth("Reset to default") + spacing + applyButtonWidth("Apply")});
    }
    return width;
}

} // namespace

Widths measure(const Platform::ProcessActionCapabilities& capabilities)
{
    return Widths{
        .buttons = ProcessActionsView::buttonsRowWidth(capabilities),
        .priority = priorityRowsWidth(capabilities),
        .padding = ImGui::GetStyle().WindowPadding.x * 2.0F,
    };
}

float render(const Context& context, const ProcessDetailsLayout::ActionsBlockLayout& layout, float rowCardHeight)
{
    if (context.actionsView == nullptr || context.priorityView == nullptr || context.processName == nullptr)
    {
        return 0.0F;
    }

    // A card like Identity's and Runtime's, its header inside (#1537). Beside them: their height, so
    // the three share their edges and the charts under the row keep theirs. Wrapped below them: as
    // tall as the content.
    const ImGuiChildFlags childFlags = layout.besideInfo ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY;
    const float cardHeight = layout.besideInfo ? std::max(rowCardHeight, 1.0F) : 0.0F;
    float neededHeight = 0.0F;
    (void) ProcessOverviewCard::render(
        "ProcessActionsBlock",
        ICON_FA_GEARS,
        "Actions",
        ImVec2(layout.width, cardHeight),
        childFlags,
        [&]
        {
            // The result and error lines wrap at the block's edge instead of running out of it.
            ImGui::PushTextWrapPos(0.0F);
            context.actionsView->renderControls(context.actions, context.capabilities, *context.processName, context.target);
            context.priorityView->render(context.actions, context.capabilities, context.currentNice, context.target);
            context.actionsView->renderResultLine();
            ImGui::PopTextWrapPos();
            // The last row's bottom (the cursor is an item spacing below it) and the bottom padding.
            const ImGuiStyle& style = ImGui::GetStyle();
            neededHeight = ImGui::GetCursorPosY() - style.ItemSpacing.y + style.WindowPadding.y;
        });
    return neededHeight;
}

} // namespace App::ProcessActionsBlock
