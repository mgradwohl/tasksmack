#include "ProcessActionsBlock.h"

#include "Platform/IProcessActions.h"
#include "ProcessActionsView.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "ProcessIoPriorityView.h"
#include "ProcessPriorityView.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"

#include <imgui.h>

#include <algorithm>

#ifdef _WIN32
#include <string> // The current class's text, measured for the Windows Priority row
#endif

namespace App::ProcessActionsBlock
{

namespace
{

/// An Apply button as ProcessPriorityView and ProcessIoPriorityView size theirs: the label, held to
/// their em floor.
[[nodiscard]] float applyButtonWidth(const char* label)
{
    return UI::DialogMetrics::computeActionButtonWidth(
        ImGui::CalcTextSize(label).x, ImGui::GetFontSize(), Detail::PRIORITY_APPLY_BUTTON_MIN_EM);
}

/// The priority rows ProcessPriorityView draws for @p capabilities: label, controls, Apply and, on
/// Windows, the current class after it.
[[nodiscard]] float priorityRowsWidth(const Platform::ProcessActionCapabilities& capabilities)
{
    const float emPx = ImGui::GetFontSize();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float width = 0.0F;
    if (capabilities.canSetPriority)
    {
#ifdef _WIN32
        // Priority [class combo] [Apply] current: <class>, the class at its longest.
        float widestCurrent = 0.0F;
        for (const Detail::WindowsPriorityClass priorityClass : Detail::SETTABLE_WINDOWS_PRIORITY_CLASSES)
        {
            const std::string current = "current: " + std::string(Detail::windowsPriorityClassName(priorityClass));
            widestCurrent = std::max(widestCurrent, ImGui::CalcTextSize(current.c_str()).x);
        }
        width = ImGui::CalcTextSize("Priority").x + spacing + (Detail::PRIORITY_CLASS_COMBO_WIDTH_EM * emPx) + spacing +
                applyButtonWidth(ICON_FA_CHECK "  Apply") + spacing + widestCurrent;
#else
        // The gradient slider row, which fits itself to the width it is given.
        width = ProcessDetailsLayout::PRIORITY_SLIDER_ROW_WIDTH_EM * emPx;
#endif
    }
    if (capabilities.canSetIoPriority)
    {
        // I/O priority [class combo] [level] [Apply], at the controls' authored widths.
        width = std::max(width,
                         ImGui::CalcTextSize("I/O priority").x + spacing + Detail::ioControlsWidth(emPx, spacing, true, 1.0F) + spacing +
                             applyButtonWidth("Apply"));
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

float render(const Context& context, const ProcessDetailsLayout::ActionsBlockLayout& layout, float rowChildHeight)
{
    if (context.actionsView == nullptr || context.priorityView == nullptr || context.processName == nullptr)
    {
        return 0.0F;
    }

    ImGui::BeginGroup();
    (void) UI::Widgets::sectionHeader(ICON_FA_GEARS, "Actions");
    // Beside Identity and Runtime: their height, so the charts under the row keep theirs. Wrapped
    // below them: as tall as the content.
    const ImGuiChildFlags childFlags =
        ImGuiChildFlags_AlwaysUseWindowPadding | (layout.besideInfo ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY);
    const float childHeight = layout.besideInfo ? std::max(rowChildHeight, 1.0F) : 0.0F;
    float neededHeight = 0.0F;
    if (ImGui::BeginChild("ProcessActionsBlock", ImVec2(layout.width, childHeight), childFlags, ImGuiWindowFlags_None))
    {
        // The result and error lines wrap at the block's edge instead of running out of it.
        ImGui::PushTextWrapPos(0.0F);
        context.actionsView->renderControls(context.capabilities, *context.processName, context.target);
        context.priorityView->render(context.actions, context.capabilities, context.currentNice, context.target);
        context.actionsView->renderResultLine();
        ImGui::PopTextWrapPos();
        // The last row's bottom (the cursor is an item spacing below it) and the bottom padding.
        const ImGuiStyle& style = ImGui::GetStyle();
        neededHeight = ImGui::GetCursorPosY() - style.ItemSpacing.y + style.WindowPadding.y;
    }
    ImGui::EndChild();
    ImGui::EndGroup();
    return neededHeight;
}

} // namespace App::ProcessActionsBlock
