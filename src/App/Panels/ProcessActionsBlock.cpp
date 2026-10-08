#include "ProcessActionsBlock.h"

#include "Platform/IProcessActions.h"
#include "ProcessActionsView.h"
#include "ProcessDetailsLayout.h"
#include "ProcessPriorityView.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"

#include <imgui.h>

#include <algorithm>

namespace App::ProcessActionsBlock
{

Widths measure(const Platform::ProcessActionCapabilities& capabilities)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float emPx = ImGui::GetFontSize();

    // The button grid as ProcessActionsView::renderButtons() sizes it with room to spare: two columns
    // of the widest label's button and the gutter, with the table's one CellPadding.x a side between them.
    float widestLabel = 0.0F;
    for (const Detail::ActionButtonSpec& button : Detail::ACTION_BUTTONS)
    {
        widestLabel = std::max(widestLabel, ImGui::CalcTextSize(button.label).x);
    }
    const float buttonWidth =
        UI::DialogMetrics::computeActionButtonWidth(widestLabel, emPx, ProcessDetailsLayout::ACTION_BUTTON_MIN_WIDTH_EM);
    const float gutter = ProcessDetailsLayout::ACTION_BUTTON_GUTTER_EM * emPx;
    const float controls = (ProcessDetailsLayout::ACTION_BUTTON_COLUMNS * (buttonWidth + gutter + style.CellPadding.x));

    return Widths{
        .controls = controls,
        .priority = hasPriorityControls(capabilities) ? ProcessDetailsLayout::ACTIONS_PRIORITY_COLUMN_WIDTH_EM * emPx : 0.0F,
        // An unbordered table pads each side of the gap between its columns by CellPadding.x.
        .columnGap = style.CellPadding.x * 2.0F,
        .padding = style.WindowPadding.x * 2.0F,
    };
}

void render(const Context& context, const Widths& widths, const ProcessDetailsLayout::ActionsBlockLayout& layout, float rowChildHeight)
{
    if (context.actionsView == nullptr || context.priorityView == nullptr || context.processName == nullptr)
    {
        return;
    }

    const auto renderControls = [&context]
    {
        context.actionsView->render(context.actions, context.capabilities, *context.processName, context.target);
    };
    const auto renderPriority = [&context]
    {
        context.priorityView->render(context.actions, context.capabilities, context.currentNice, context.target);
    };

    ImGui::BeginGroup();
    (void) UI::Widgets::sectionHeader(ICON_FA_GEARS, "Actions");
    // Beside Identity and Runtime: their height, so the charts under the row keep theirs. Wrapped
    // below them: as tall as the content.
    const ImGuiChildFlags childFlags =
        ImGuiChildFlags_AlwaysUseWindowPadding | (layout.besideInfo ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY);
    const float childHeight = layout.besideInfo ? std::max(rowChildHeight, 1.0F) : 0.0F;
    if (ImGui::BeginChild("ProcessActionsBlock", ImVec2(layout.width, childHeight), childFlags, ImGuiWindowFlags_None))
    {
        // The result and error lines wrap at the block's edge instead of running out of it.
        ImGui::PushTextWrapPos(0.0F);
        if (layout.columnsSideBySide && hasPriorityControls(context.capabilities) &&
            ImGui::BeginTable("ProcessActionsBlockColumns", 2, ImGuiTableFlags_SizingFixedFit))
        {
            // The buttons at the width measure() gave them; the priority control takes the rest.
            ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthFixed, widths.controls);
            ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            renderControls();
            ImGui::TableNextColumn();
            renderPriority();
            ImGui::EndTable();
        }
        else
        {
            renderControls();
            renderPriority();
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::EndGroup();
}

} // namespace App::ProcessActionsBlock
