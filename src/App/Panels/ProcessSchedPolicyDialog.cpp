#include "ProcessSchedPolicyDialog.h"

#include "Platform/IProcessActions.h"
#include "Platform/SchedPolicy.h"
#include "ProcessDetailsPanel_ActionHelpers.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

namespace
{

// The "###" part keeps the popup's ID fixed while its title names the process.
constexpr const char* POPUP_ID = "###SetSchedPolicy";
constexpr const char* APPLY_LABEL = "Apply";
constexpr const char* CANCEL_LABEL = "Cancel";

// Floor on the footer's buttons, in ems, as for the other action dialogs (ProcessActionConfirm).
constexpr float BUTTON_MIN_EM = 11.25F;
// The dialog's width in ems: room for the longest description on two lines.
constexpr float DIALOG_WIDTH_EM = 34.0F;

/// How the line above the choices names a policy the dialog doesn't offer, or nothing for one it does.
[[nodiscard]] std::string_view unofferedPolicyName(Platform::SchedPolicy policy) noexcept
{
    switch (policy)
    {
    case Platform::SchedPolicy::Fifo:
        return "the real-time FIFO policy";
    case Platform::SchedPolicy::RoundRobin:
        return "the real-time round robin policy";
    case Platform::SchedPolicy::Deadline:
        return "the Deadline policy";
    case Platform::SchedPolicy::Unknown:
    case Platform::SchedPolicy::Normal:
    case Platform::SchedPolicy::Batch:
    case Platform::SchedPolicy::Idle:
        break;
    }
    return {};
}

} // namespace

void ProcessSchedPolicyDialog::open(Platform::ProcessTarget target, std::string name, Platform::SchedPolicy current)
{
    m_Target = target;
    m_Name = std::move(name);
    m_Current = current;
    // Only a policy the dialog offers is chosen for the user; see open()'s comment in the header.
    m_Choice = Platform::isSettableSchedPolicy(current) ? std::optional<Platform::SchedPolicy>{current} : std::nullopt;
    m_ShowRequested = true;
    m_DismissRequested = false; // a dismiss() the last dialog never consumed must not close this one
}

void ProcessSchedPolicyDialog::choose(Platform::SchedPolicy policy) noexcept
{
    if (Platform::isSettableSchedPolicy(policy))
    {
        m_Choice = policy;
    }
}

std::optional<Platform::SchedPolicy> ProcessSchedPolicyDialog::render()
{
    if (m_ShowRequested)
    {
        ImGui::OpenPopup(POPUP_ID);
        m_ShowRequested = false;
    }
    m_Open = ImGui::IsPopupOpen(POPUP_ID);
    if (!m_Open)
    {
        return std::nullopt;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const std::string title = std::format("{}  Set scheduling policy for {} (PID {}){}", ICON_FA_CLOCK, m_Name, m_Target.pid, POPUP_ID);
    UI::Widgets::centerNextDialog(m_LastWorkSize);
    const float width = std::min((DIALOG_WIDTH_EM * ImGui::GetFontSize()) + (ImGui::GetStyle().WindowPadding.x * 2.0F),
                                 UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0F));
    UI::Widgets::setNextDialogSizeConstraints(ImVec2(UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.x),
                                                     UI::DialogMetrics::computeDialogMaxExtent(viewport->WorkSize.y)));
    std::optional<Platform::SchedPolicy> applied;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        return applied;
    }
    UI::Widgets::keepCurrentWindowInViewport();
    if (std::exchange(m_DismissRequested, false))
    {
        m_Open = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return applied;
    }
    // Escape cancels, as Cancel does: the policy is left alone (#1944). Read before the widgets below take keys.
    const bool escaped = UI::Widgets::dialogEscapePressed();
    renderChoices();
    applied = renderFooter(escaped);
    ImGui::EndPopup();
    return applied;
}

void ProcessSchedPolicyDialog::renderChoices()
{
    // What the dialog changes, a warning when the process runs under a policy it doesn't offer (real time or
    // Deadline: applying takes it off that), then one radio button per offered policy with what it does.
    const auto& theme = UI::Theme::get();

    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted("How the kernel schedules this process. Every thread is changed, and threads it starts later inherit it.");
    if (const std::string_view unoffered = unofferedPolicyName(m_Current); !unoffered.empty())
    {
        ImGui::TextColored(theme.scheme().textWarning,
                           ICON_FA_TRIANGLE_EXCLAMATION "  It runs under %.*s now. Applying a choice below takes it off that policy.",
                           static_cast<int>(unoffered.size()),
                           unoffered.data());
    }
    ImGui::Spacing();
    for (const SchedPolicyChoice& option : SCHED_POLICY_CHOICES)
    {
        if (ImGui::RadioButton(option.label, m_Choice == option.policy))
        {
            choose(option.policy);
        }
        ImGui::Indent();
        ImGui::TextColored(theme.scheme().textMuted, "%s", option.description);
        ImGui::Unindent();
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
}

std::optional<Platform::SchedPolicy> ProcessSchedPolicyDialog::renderFooter(bool escaped)
{
    // Apply (disabled until a policy is chosen) returns the choice and closes; Cancel, or Escape pressed this
    // frame (@p escaped), closes without one.
    const auto& theme = UI::Theme::get();
    const bool chosen = canApply();
    const UI::Widgets::ButtonFills applyFills{
        .resting = theme.scheme().successButton,
        .hovered = theme.scheme().successButtonHovered,
        .pressed = theme.scheme().successButtonActive,
    };
    const UI::Widgets::DialogFooterButton applyButton{
        .label = APPLY_LABEL,
        .fills = &applyFills,
        .tooltip = chosen ? nullptr : "Choose a policy",
        .disabled = !chosen,
    };
    const UI::Widgets::DialogFooterButton cancelButton{.label = CANCEL_LABEL, .fills = nullptr, .tooltip = nullptr};
    UI::Widgets::DialogFooterAction pressed =
        UI::Widgets::dialogFooter(applyButton, cancelButton, UI::Widgets::footerButtonWidth({APPLY_LABEL, CANCEL_LABEL}, BUTTON_MIN_EM));
    if (escaped)
    {
        pressed = UI::Widgets::DialogFooterAction::Secondary;
    }
    std::optional<Platform::SchedPolicy> applied;
    switch (pressed)
    {
    case UI::Widgets::DialogFooterAction::Primary:
        if (chosen)
        {
            applied = m_Choice;
            m_Open = false;
            ImGui::CloseCurrentPopup();
        }
        break;
    case UI::Widgets::DialogFooterAction::Secondary:
        m_Open = false;
        ImGui::CloseCurrentPopup();
        break;
    case UI::Widgets::DialogFooterAction::None:
    case UI::Widgets::DialogFooterAction::Leading:
        break;
    }
    return applied;
}

Detail::ActionResultMessage schedPolicyResultMessage(const std::string& name,
                                                     std::int32_t pid,
                                                     Platform::SchedPolicy policy,
                                                     const Platform::ProcessActionResult& result)
{
    if (!result.success)
    {
        return {.ok = false, .text = result.errorMessage};
    }
    std::string text = std::format("Scheduling policy of {} (PID {}) set to {}", name, pid, Platform::schedPolicyText(policy, 0).view());
    if (!result.successNote.empty())
    {
        // Threads that kept a real-time policy the dialog didn't show (a real-time worker under a Normal main thread)
        text += "; ";
        text += result.successNote;
    }
    return {.ok = true, .text = std::move(text)};
}

} // namespace App
