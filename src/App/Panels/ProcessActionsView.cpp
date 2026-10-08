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
    renderControls(actions, capabilities, processName, target);
}

void ProcessActionsView::renderControls(Platform::IProcessActions* actions,
                                        const Platform::ProcessActionCapabilities& capabilities,
                                        const std::string& processName,
                                        const Platform::ProcessTarget& target)
{
    // No name, PID or header of its own: the view sits in the Overview's Actions block, under that
    // block's header and beside the Identity block that names the process (#1493). Its result line
    // goes under the block's last row (renderResultLine()).
    const bool anyButton = renderButtons(capabilities, processName, target);
    renderSyscallTraceButton(actions, capabilities, target, anyButton);
}

void ProcessActionsView::renderSyscallTraceButton(Platform::IProcessActions* actions,
                                                  const Platform::ProcessActionCapabilities& capabilities,
                                                  const Platform::ProcessTarget& target,
                                                  bool afterButtons)
{
    // Its state comes from capabilities found once, when the platform's actions were made: nothing is
    // looked up on PATH per frame.
    const Detail::SyscallTraceButton button = Detail::syscallTraceButton(capabilities, target);
    if (button.state == Detail::SyscallTraceButtonState::Hidden)
    {
        return;
    }

    // The end of the button row at its own label's width (#182), not the others' shared width, which
    // it would stretch; on a row of its own when it does not fit after them.
    const float width = syscallTraceButtonWidth(capabilities);
    if (afterButtons)
    {
        // Decided before SameLine(): SameLine() then NewLine() would leave the row's extent at the
        // SameLine() position, an item spacing past the last button, and widen the block's content.
        const float rowEnd = ImGui::GetItemRectMax().x;
        const float regionEnd = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (rowEnd + ImGui::GetStyle().ItemSpacing.x + width <= regionEnd)
        {
            ImGui::SameLine();
        }
    }
    const bool disabled = button.state == Detail::SyscallTraceButtonState::Disabled;
    ImGui::BeginDisabled(disabled);
    const bool pressed = ImGui::Button(Detail::SYSCALL_TRACE_LABEL, ImVec2(width, 0.0F));
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

float ProcessActionsView::syscallTraceButtonWidth(const Platform::ProcessActionCapabilities& capabilities)
{
    if (capabilities.syscallTrace == Platform::SyscallTraceAvailability::Unsupported)
    {
        return 0.0F;
    }
    return ProcessDetailsLayout::computeActionButtonWidth(ImGui::CalcTextSize(Detail::SYSCALL_TRACE_LABEL).x,
                                                          ImGui::GetStyle().FramePadding.x);
}

float ProcessActionsView::buttonsRowWidth(const Platform::ProcessActionCapabilities& capabilities)
{
    std::size_t count = 0;
    for (const Detail::ActionButtonSpec& button : Detail::ACTION_BUTTONS)
    {
        count += Detail::isActionAvailable(capabilities, button.action) ? 1U : 0U;
    }
    const float actionRow =
        ProcessDetailsLayout::computeActionButtonRowWidth(actionButtonWidth(capabilities), count, ImGui::GetStyle().ItemSpacing.x);
    return ProcessDetailsLayout::computeActionButtonsWidth(actionRow, syscallTraceButtonWidth(capabilities));
}

bool ProcessActionsView::renderButtons(const Platform::ProcessActionCapabilities& capabilities,
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
    return !first;
}

} // namespace App
