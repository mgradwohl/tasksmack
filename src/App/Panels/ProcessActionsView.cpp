#include "ProcessActionsView.h"

#include "Platform/IProcessActions.h"
#include "ProcessActionConfirm.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "UI/ChromeWidgets.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <string>

namespace App
{

void ProcessActionsView::render(Platform::IProcessActions* actions,
                                const Platform::ProcessActionCapabilities& capabilities,
                                const std::string& processName,
                                const Platform::ProcessTarget& target)
{
    ImGui::Text("%s (PID %d)", processName.c_str(), target.pid);
    ImGui::Spacing();

    // Section: Process Control
    (void) UI::Widgets::sectionHeader(ICON_FA_GEARS, "Process Control");
    ImGui::Spacing();

    renderResultFeedback();
    renderConfirmDialog(actions, target);
    renderButtons(capabilities, processName, target);
    renderSyscallTraceButton(actions, capabilities, target);
}

void ProcessActionsView::renderSyscallTraceButton(Platform::IProcessActions* actions,
                                                  const Platform::ProcessActionCapabilities& capabilities,
                                                  const Platform::ProcessTarget& target)
{
    // Its state comes from capabilities found once, when the platform's actions were made: nothing is
    // looked up on PATH per frame.
    const Detail::SyscallTraceButton button = Detail::syscallTraceButton(capabilities, target);
    if (button.state == Detail::SyscallTraceButtonState::Hidden)
    {
        return;
    }

    ImGui::Spacing();
    const bool disabled = button.state == Detail::SyscallTraceButtonState::Disabled;
    ImGui::BeginDisabled(disabled);
    const bool pressed = ImGui::Button(Detail::SYSCALL_TRACE_LABEL);
    ImGui::EndDisabled();
    if (pressed && !disabled)
    {
        launchSyscallTrace(actions, target);
    }
    // A disabled button still explains itself on hover.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        constexpr float TOOLTIP_WIDTH_EM = 30.0F;
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * TOOLTIP_WIDTH_EM, 0.0F));
        if (ImGui::BeginTooltip())
        {
            ImGui::TextWrapped("%s", button.tooltip);
            ImGui::EndTooltip();
        }
    }
}

void ProcessActionsView::renderResultFeedback() const
{
    if (m_LastResult.empty())
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    // The colour comes from the result's flag, not from searching its text for "Error" (#1203).
    const ImVec4 color = m_LastResult.ok ? theme.scheme().textSuccess : theme.scheme().textError;
    ImGui::TextColored(color, "%s", m_LastResult.text.c_str());
    ImGui::Spacing();
}

void ProcessActionsView::renderConfirmDialog(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget)
{
    // The same dialog the Processes table's row menu confirms with (#1209). It names, and a confirm
    // acts on, the process captured when the button was pressed; after a selection change it is
    // closed unconfirmed instead, so it can never act on another process.
    // Every outcome but None ends the confirm: dispatchConfirmed() clears it after acting, and a
    // cancel or dismissal clears it without acting, so nothing stale is left to replay.
    const bool dismiss = takeDismiss(liveTarget);
    switch (ProcessActionConfirm::render(
        m_ShowConfirmDialog, m_ConfirmAction, m_ConfirmTarget.processName, m_ConfirmTarget.target.pid, dismiss))
    {
    case ProcessActionConfirm::Outcome::Confirmed:
        dispatchConfirmed(actions);
        break;
    case ProcessActionConfirm::Outcome::Cancelled:
        cancelConfirm();
        break;
    case ProcessActionConfirm::Outcome::None:
        break;
    }
}

void ProcessActionsView::renderButtons(const Platform::ProcessActionCapabilities& capabilities,
                                       const std::string& processName,
                                       const Platform::ProcessTarget& target)
{
    // One width for all four, from the widest label and the font, capped to the pane (#949). See
    // ProcessDetailsLayout::computeActionButtonWidth() for why it is no longer a fixed 180px.
    const float emPx = ImGui::GetFontSize();
    const float gutter = ProcessDetailsLayout::ACTION_BUTTON_GUTTER_EM * emPx;
    float widestLabel = 0.0F;
    for (const Detail::ActionButtonSpec& button : Detail::ACTION_BUTTONS)
    {
        widestLabel = std::max(widestLabel, ImGui::CalcTextSize(button.label).x);
    }
    // Per-column overhead is the gutter plus one CellPadding.x, not two. This table has no inner
    // border, so ImGui does not pad inside each cell: it puts CellPadding.x on each side of the gap
    // *between* columns. Two columns have one gap, so the table is 2 * (width + gutter) plus
    // 2 * CellPadding.x in total -- one CellPadding.x per column.
    const float buttonWidth = ProcessDetailsLayout::computeActionButtonWidth(
        widestLabel, emPx, ImGui::GetContentRegionAvail().x, gutter + ImGui::GetStyle().CellPadding.x);
    constexpr float BUTTON_HEIGHT = 0.0F; // Use default height
    const ImVec2 buttonSize(buttonWidth, BUTTON_HEIGHT);

    // Terminate and Kill end the process, so they are drawn in the theme's danger colour, apart from
    // Suspend and Resume, which can be undone (#1273).
    const auto& theme = UI::Theme::get();

    // Use a table for consistent alignment: a 2x2 grid, Terminate and Kill, then Suspend and Resume.
    if (!ImGui::BeginTable("ActionButtons", 2, ImGuiTableFlags_SizingFixedFit))
    {
        return;
    }
    ImGui::TableSetupColumn("Col1", ImGuiTableColumnFlags_WidthFixed, buttonWidth + gutter);
    ImGui::TableSetupColumn("Col2", ImGuiTableColumnFlags_WidthFixed, buttonWidth + gutter);

    for (std::size_t index = 0; index < Detail::ACTION_BUTTONS.size(); ++index)
    {
        const Detail::ActionButtonSpec& button = Detail::ACTION_BUTTONS.at(index);
        if (index % Detail::ACTION_BUTTON_GRID_COLUMNS == 0)
        {
            ImGui::TableNextRow();
        }
        ImGui::TableNextColumn();
        // An action the platform cannot run leaves its cell empty.
        if (!Detail::isActionAvailable(capabilities, button.action))
        {
            continue;
        }

        const bool pressed = Detail::isDestructiveAction(button.action)
                               ? UI::Widgets::filledButton(button.label,
                                                           buttonSize,
                                                           ProcessActionConfirm::dangerButtonFills(),
                                                           theme.scheme().textPrimary,
                                                           theme.scheme().windowBg)
                               : ImGui::Button(button.label, buttonSize);
        if (pressed)
        {
            requestAction(button.action, target, processName);
        }
        if (ImGui::IsItemHovered())
        {
            // Kill also has a key (#170)
            if (button.action == Detail::ProcessAction::Kill)
            {
                ImGui::SetTooltip("%s (F9)", button.tooltip);
            }
            else
            {
                ImGui::SetTooltip("%s", button.tooltip);
            }
        }
    }

    ImGui::EndTable();
}

} // namespace App
