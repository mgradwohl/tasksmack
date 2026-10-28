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
#include <cmath>

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
        // The class slider at the narrowest that shows all five stop names (#1538), and Apply under it.
        // Runtime's Priority row shows the current class, not this row (#1537).
        width =
            std::max(Detail::discretePrioritySliderMinWidth(Detail::WINDOWS_PRIORITY_SLIDER), applyButtonWidth(ICON_FA_CHECK "  Apply"));
#else
        // The gradient slider row, which fits itself to the width it is given.
        width = ProcessDetailsLayout::PRIORITY_SLIDER_ROW_WIDTH_EM * emPx;
#endif
        if (ProcessPriorityView::labelsRows(capabilities))
        {
            // Its "Priority" row label, drawn only beside the I/O priority row (#1874).
            width = std::max(width, ImGui::CalcTextSize("Priority").x);
        }
    }
    if (capabilities.canSetIoPriority)
    {
        // "I/O priority", its slider under it at the nice slider's row width (it fits itself to the width
        // it is given, as that one does), and [Reset to default] [Apply] under the slider (#1540).
        width = std::max({width,
                          ImGui::CalcTextSize("I/O priority").x,
                          ProcessDetailsLayout::PRIORITY_SLIDER_ROW_WIDTH_EM * emPx,
                          applyButtonWidth("Reset to default") + spacing + applyButtonWidth(ICON_FA_CHECK "  Apply")});
    }
    return width;
}

/// Draws @p rows, a card's content under its header, and returns what the card needs down for them:
/// from its top to their bottom, and the bottom padding. In a card given a fixed height (@p centred)
/// they are centred in the room left under the header (#1874), from their height the frame before,
/// which the card keeps in its state storage: a card on a row taller than it -- Actions' one row of
/// buttons beside Identity and Runtime -- no longer leaves all its spare room in one block under them.
/// The height returned leaves that offset out, so the row's height (computeRowCardHeight()) does not
/// feed on it.
template<typename Rows> [[nodiscard]] float renderRows(bool centred, const Rows& rows)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID("##RowsHeight");
    const float top = ImGui::GetCursorPosY();
    const float offset =
        centred ? ProcessDetailsLayout::computeCentredRowsOffset(ImGui::GetContentRegionAvail().y, storage->GetFloat(key, 0.0F)) : 0.0F;
    if (offset > 0.0F)
    {
        ImGui::SetCursorPosY(top + offset);
    }
    rows();
    // The last row's bottom: the cursor is an item spacing below it.
    const float rowsHeight = std::max(0.0F, ImGui::GetCursorPosY() - style.ItemSpacing.y - (top + offset));
    storage->SetFloat(key, rowsHeight);
    return top + rowsHeight + style.WindowPadding.y;
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

Heights render(const Context& context, const ProcessDetailsLayout::TopRowLayout& layout, float cardHeight)
{
    Heights heights;
    if (context.actionsView == nullptr || context.priorityView == nullptr || context.processName == nullptr)
    {
        return heights;
    }

    // Cards like Identity's and Runtime's, each with its header inside (#1537). Given the row's height
    // they take it, so the row's cards share their edges; otherwise each is as tall as its content.
    const bool fixedHeight = std::isfinite(cardHeight) && cardHeight > 0.0F;
    const ImGuiChildFlags childFlags = fixedHeight ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY;
    const float height = fixedHeight ? cardHeight : 0.0F;

    const bool drawActions = layout.actionsWidth > 0.0F;
    if (drawActions)
    {
        (void) ProcessOverviewCard::render("ProcessActionsBlock",
                                           ICON_FA_GEARS,
                                           "Actions",
                                           ImVec2(layout.actionsWidth, height),
                                           childFlags,
                                           [&]
                                           {
                                               heights.actions = renderRows(fixedHeight,
                                                                            [&]
                                                                            {
                                                                                // The result line wraps at the card's edge
                                                                                // instead of running out of it.
                                                                                ImGui::PushTextWrapPos(0.0F);
                                                                                context.actionsView->renderControls(context.actions,
                                                                                                                    context.capabilities,
                                                                                                                    *context.processName,
                                                                                                                    context.target,
                                                                                                                    context.currentAffinity,
                                                                                                                    context.efficiencyMode);
                                                                                context.actionsView->renderResultLine();
                                                                                ImGui::PopTextWrapPos();
                                                                            });
                                           });
    }

    if (layout.priorityWidth > 0.0F)
    {
        if (drawActions && layout.priorityBesideActions)
        {
            ImGui::SameLine();
        }
        (void) ProcessOverviewCard::render(
            "ProcessPriorityBlock",
            ICON_FA_GAUGE_HIGH,
            "Priority",
            ImVec2(layout.priorityWidth, height),
            childFlags,
            [&]
            {
                // With the priority row alone (Windows) the header is its label, and so
                // shows the current priority on hover, as the row label did (#1874).
                if (context.capabilities.canSetPriority && !ProcessPriorityView::labelsRows(context.capabilities))
                {
                    ProcessPriorityView::setCurrentPriorityTooltip(context.currentNice);
                }
                heights.priority =
                    renderRows(fixedHeight,
                               [&]
                               {
                                   // The error lines wrap at the card's edge too.
                                   ImGui::PushTextWrapPos(0.0F);
                                   context.priorityView->render(context.actions, context.capabilities, context.currentNice, context.target);
                                   ImGui::PopTextWrapPos();
                               });
            });
    }
    return heights;
}

} // namespace App::ProcessActionsBlock
