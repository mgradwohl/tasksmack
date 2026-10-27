#include "ProcessActionsView.h"

#include "AuthorizeWording.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "ProcessActionConfirm.h"
#include "ProcessAffinityDialog.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "ProcessDumpView.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <thread>

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
                                        const Platform::ProcessTarget& target,
                                        const Platform::CpuAffinity* currentAffinity)
{
    // No name, PID or header of its own: the view sits in the Overview's Actions block, under that
    // block's header and beside the Identity block that names the process (#1493). Its result line
    // goes under the block's last row (renderResultLine()).
    const bool anyButton = renderButtons(capabilities, processName, target);
    renderSyscallTraceButton(actions, capabilities, target, anyButton);
    const bool anyBeforeAffinity = anyButton || capabilities.syscallTrace != Platform::SyscallTraceAvailability::Unsupported;
    renderSetAffinityButton(actions, capabilities, processName, target, currentAffinity, anyBeforeAffinity);
    renderCreateDumpButton(actions, capabilities, processName, target, anyBeforeAffinity || capabilities.canSetAffinity);
}

void ProcessActionsView::renderCreateDumpButton(Platform::IProcessActions* actions,
                                                const Platform::ProcessActionCapabilities& capabilities,
                                                const std::string& processName,
                                                const Platform::ProcessTarget& target,
                                                bool afterButtons)
{
    if (!DumpDetail::offersCreateDump(capabilities, 0))
    {
        return;
    }
    // As the trace and affinity buttons: on the row at its own width when it fits, else on a row of its own.
    const float width = createDumpButtonWidth(capabilities);
    if (afterButtons)
    {
        const float rowEnd = ImGui::GetItemRectMax().x;
        const float regionEnd = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (rowEnd + ImGui::GetStyle().ItemSpacing.x + width <= regionEnd)
        {
            ImGui::SameLine();
        }
    }
    const bool disabled = target.pid <= 0 || m_DumpView.active();
    ImGui::BeginDisabled(disabled);
    const bool pressed = ImGui::Button(DumpDetail::CREATE_DUMP_LABEL, ImVec2(width, 0.0F));
    ImGui::EndDisabled();
    if (pressed && !disabled)
    {
        // The process is captured now; the confirm and the dump act only on it.
        // The remembered folder is checked on the confirm's worker, never here (#3155).
        m_DumpView.requestInRememberedFolder(target, processName, DumpDetail::rememberedDumpFolder(), actions);
    }
    ImGui::SetItemTooltip("%s", DumpDetail::CREATE_DUMP_TOOLTIP);
}

void ProcessActionsView::renderSetAffinityButton(const Platform::IProcessActions* actions,
                                                 const Platform::ProcessActionCapabilities& capabilities,
                                                 const std::string& processName,
                                                 const Platform::ProcessTarget& target,
                                                 const Platform::CpuAffinity* currentAffinity,
                                                 bool afterButtons)
{
    if (!capabilities.canSetAffinity)
    {
        return;
    }
    // As the trace button: on the row at its own width when it fits, else on a row of its own.
    const float width = setAffinityButtonWidth(capabilities);
    if (afterButtons)
    {
        const float rowEnd = ImGui::GetItemRectMax().x;
        const float regionEnd = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (rowEnd + ImGui::GetStyle().ItemSpacing.x + width <= regionEnd)
        {
            ImGui::SameLine();
        }
    }
    const bool noTarget = target.pid <= 0;
    ImGui::BeginDisabled(noTarget || m_ActionRun.busy());
    const bool pressed = ImGui::Button(Detail::SET_AFFINITY_LABEL, ImVec2(width, 0.0F));
    ImGui::EndDisabled();
    if (pressed && !noTarget)
    {
        // The process is captured now; the dialog acts only on it (renderAffinityDialog()).
        m_AffinityDialog.open(target,
                              processName,
                              currentAffinity != nullptr ? *currentAffinity : Platform::CpuAffinity{},
                              static_cast<std::size_t>(std::thread::hardware_concurrency()),
                              // Read once, on the press: the groups and core kinds to arrange the boxes by (#1881)
                              actions != nullptr ? actions->affinityLayout() : Platform::CpuAffinityLayout{});
    }
    ImGui::SetItemTooltip("Choose the CPUs this process may run on");
}

void ProcessActionsView::renderAffinityDialog(Platform::IProcessActions* actions, const Platform::ProcessTarget& liveTarget)
{
    if (m_AffinityDialog.isPending() && !Detail::isSameProcessTarget(m_AffinityDialog.target(), liveTarget))
    {
        m_AffinityDialog.dismiss(); // another process is shown now: never act on the old one
    }
    const std::optional<Platform::CpuAffinity> cpus = m_AffinityDialog.render();
    if (!cpus.has_value())
    {
        return;
    }
    const Platform::ProcessTarget target = m_AffinityDialog.target();
    runAction(
        actions,
        std::format("Setting the CPU affinity of PID {}...", target.pid),
        [actions, target, cpus = *cpus] { return actions->setAffinity(target, cpus); },
        [name = m_AffinityDialog.name(), pid = target.pid, cpus = *cpus](const Platform::ProcessActionResult& result)
        { return affinityResultMessage(name, pid, cpus, result); },
        Platform::AuthorizedAction{.kind = Platform::AuthorizedAction::Kind::SetAffinity, .target = target, .cpus = *cpus});
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
    const bool disabled = button.state == Detail::SyscallTraceButtonState::Disabled || m_ActionRun.busy();
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

void ProcessActionsView::renderResultFeedback()
{
    const auto& theme = UI::Theme::get();
    if (m_ActionRun.busy() && m_RunGeneration == m_Generation) // not another process's action (#2389)
    {
        ImGui::TextColored(theme.scheme().textMuted, "%s", m_ActionRun.pendingText().c_str()); // #2305
        ImGui::Spacing();
        return;
    }
    if (m_LastResult.empty())
    {
        return;
    }

    // The colour comes from the result's flag, not from searching its text for "Error" (#1203).
    const ImVec4 color = m_LastResult.ok ? theme.scheme().textSuccess : theme.scheme().textError;
    ImGui::TextColored(color, "%s", m_LastResult.text.c_str());
    // A permission refusal an administrator could redo (#2308): the shield and the words, never colour alone.
    if (offersAuthenticateAndRetry())
    {
        if (ImGui::SmallButton(AuthorizeWording::RETRY_LABEL))
        {
            retryAuthorized();
        }
        ImGui::SetItemTooltip("%s", AuthorizeWording::RETRY_TOOLTIP);
    }
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

float ProcessActionsView::setAffinityButtonWidth(const Platform::ProcessActionCapabilities& capabilities)
{
    if (!capabilities.canSetAffinity)
    {
        return 0.0F;
    }
    return ProcessDetailsLayout::computeActionButtonWidth(ImGui::CalcTextSize(Detail::SET_AFFINITY_LABEL).x,
                                                          ImGui::GetStyle().FramePadding.x);
}

float ProcessActionsView::createDumpButtonWidth(const Platform::ProcessActionCapabilities& capabilities)
{
    if (!DumpDetail::offersCreateDump(capabilities, 0))
    {
        return 0.0F;
    }
    return ProcessDetailsLayout::computeActionButtonWidth(ImGui::CalcTextSize(DumpDetail::CREATE_DUMP_LABEL).x,
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
    // The trace, affinity and dump buttons each take a row of their own when they don't fit after the others.
    return ProcessDetailsLayout::computeActionButtonsWidth(
        actionRow,
        std::max({syscallTraceButtonWidth(capabilities), setAffinityButtonWidth(capabilities), createDumpButtonWidth(capabilities)}));
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

    // Not while an action runs on the worker (#2305): one at a time.
    ImGui::BeginDisabled(m_ActionRun.busy());
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
    ImGui::EndDisabled();
    return !first;
}

} // namespace App
