#include "ProcessActionsView.h"

#include "Platform/IProcessActions.h"
#include "ProcessActionConfirm.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
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
    renderConfirmation(actions, target);
    renderControls(capabilities, processName, target);
}

void ProcessActionsView::renderControls(const Platform::ProcessActionCapabilities& capabilities,
                                        const std::string& processName,
                                        const Platform::ProcessTarget& target)
{
    // No name, PID or header of its own: the view sits in the Overview's Actions block, under that
    // block's header and beside the Identity block that names the process (#1493). Its result line
    // goes under the block's last row (renderResultLine()).
    renderButtons(capabilities, processName, target);
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

namespace
{

/// The buttons' shared width: the widest label among those @p capabilities show, padded (#1493).
[[nodiscard]] float actionButtonWidth(const Platform::ProcessActionCapabilities& capabilities)
{
    float widestLabel = 0.0F;
    for (const Detail::ActionButtonSpec& button : Detail::ACTION_BUTTONS)
    {
        if (Detail::isActionAvailable(capabilities, button.action))
        {
            widestLabel = std::max(widestLabel, ImGui::CalcTextSize(button.label).x);
        }
    }
    return ProcessDetailsLayout::computeActionButtonWidth(widestLabel, ImGui::GetStyle().FramePadding.x);
}

} // namespace

float ProcessActionsView::buttonsRowWidth(const Platform::ProcessActionCapabilities& capabilities)
{
    std::size_t count = 0;
    for (const Detail::ActionButtonSpec& button : Detail::ACTION_BUTTONS)
    {
        count += Detail::isActionAvailable(capabilities, button.action) ? 1U : 0U;
    }
    return ProcessDetailsLayout::computeActionButtonRowWidth(actionButtonWidth(capabilities), count, ImGui::GetStyle().ItemSpacing.x);
}

void ProcessActionsView::renderButtons(const Platform::ProcessActionCapabilities& capabilities,
                                       const std::string& processName,
                                       const Platform::ProcessTarget& target)
{
    // One row of equal buttons at their labels' width (#1493): Terminate and Kill, then Suspend and
    // Resume where the platform has them. A button that would not fit on the row starts a new one, so
    // none is clipped in a narrow pane. An action the platform cannot run has no button.
    const ImVec2 buttonSize(actionButtonWidth(capabilities), 0.0F);

    // Terminate and Kill end the process, so they are drawn in the theme's danger colour, apart from
    // Suspend and Resume, which can be undone (#1273).
    const auto& theme = UI::Theme::get();

    bool first = true;
    for (const Detail::ActionButtonSpec& button : Detail::ACTION_BUTTONS)
    {
        if (!Detail::isActionAvailable(capabilities, button.action))
        {
            continue;
        }
        if (!first)
        {
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < buttonSize.x)
            {
                ImGui::NewLine();
            }
        }
        first = false;

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
}

} // namespace App
