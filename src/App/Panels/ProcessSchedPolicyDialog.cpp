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
constexpr const char* SET_LABEL = "Set";   // The real-time confirm step's primary button (#3219)
constexpr const char* BACK_LABEL = "Back"; // ... and its way back to the choices
constexpr const char* PRIORITY_SLIDER_ID = "##RtPriority";

// What the dialog says while FIFO or RR is chosen (#3219): what real time risks, the kernel's RT throttling that
// limits the damage, and RR over FIFO when the CPU is shared.
constexpr const char* REALTIME_WARNING =
    "A real-time process runs ahead of every normal one, so one that spins can starve everything else, your desktop and "
    "TaskSmack included. The kernel's RT throttling (/proc/sys/kernel/sched_rt_runtime_us, by default 95% of each second) "
    "leaves the rest a little time to recover. When it shares the CPU with other real-time work, prefer RR over FIFO.";
constexpr const char* REALTIME_MAY_BE_REFUSED =
    "TaskSmack lacks CAP_SYS_NICE, so the kernel may refuse this: it allows real time up to the process's RLIMIT_RTPRIO "
    "(0 unless one was granted, as an audio group often is), or a priority lower than one it already runs at. Where the "
    "TaskSmack helper is installed, a refusal offers Authenticate and retry.";
constexpr const char* OWN_PROCESS_NO_REALTIME = "This is TaskSmack itself, which never runs real time: its window would stop responding.";

// Floor on the footer's buttons, in ems, as for the other action dialogs (ProcessActionConfirm).
constexpr float BUTTON_MIN_EM = 11.25F;
// The dialog's width in ems: room for the longest description on two lines.
constexpr float DIALOG_WIDTH_EM = 34.0F;

/// How the line above the choices names a policy the dialog doesn't offer, or nothing for one it does.
[[nodiscard]] std::string_view unofferedPolicyName(Platform::SchedPolicy policy) noexcept
{
    switch (policy)
    {
    case Platform::SchedPolicy::Deadline:
        return "the Deadline policy";
    case Platform::SchedPolicy::Unknown:
    case Platform::SchedPolicy::Normal:
    case Platform::SchedPolicy::Batch:
    case Platform::SchedPolicy::Idle:
    case Platform::SchedPolicy::Fifo:
    case Platform::SchedPolicy::RoundRobin:
        break;
    }
    return {};
}

[[nodiscard]] constexpr std::int32_t clampRtPriority(std::int64_t priority) noexcept
{
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>(priority, Platform::MIN_REALTIME_SCHED_PRIORITY, Platform::MAX_REALTIME_SCHED_PRIORITY));
}

} // namespace

void ProcessSchedPolicyDialog::open(Platform::ProcessTarget target,
                                    std::string name,
                                    Platform::SchedPolicy current,
                                    std::uint32_t currentRtPriority,
                                    SchedPolicyDialogOptions options)
{
    m_Target = target;
    m_Name = std::move(name);
    m_Current = current;
    m_OwnProcess = options.ownPid > 0 && target.pid == options.ownPid;
    m_RealtimeNeedsNoPassword = options.realtimeNeedsNoPassword;
    // A real-time process opens at its own priority; anything else, or one whose priority wasn't read (0), at the middle.
    m_CurrentRtPriorityUnread = Platform::isRealtimeSchedPolicy(current) && currentRtPriority == 0;
    m_RtPriority = Platform::isRealtimeSchedPolicy(current) && !m_CurrentRtPriorityUnread ? clampRtPriority(currentRtPriority)
                                                                                          : DEFAULT_REALTIME_PRIORITY;
    // Only a policy the dialog offers, at a priority it read, is chosen for the user; see open()'s comment in the header.
    m_Choice = std::nullopt;
    if (!m_CurrentRtPriorityUnread)
    {
        choose(current);
    }
    m_Confirming = false;
    m_ShowRequested = true;
    m_DismissRequested = false; // a dismiss() the last dialog never consumed must not close this one
}

void ProcessSchedPolicyDialog::choose(Platform::SchedPolicy policy) noexcept
{
    if (!Platform::isSettableSchedPolicy(policy) || (Platform::isRealtimeSchedPolicy(policy) && !offersRealtime()))
    {
        return;
    }
    m_Choice = policy;
}

void ProcessSchedPolicyDialog::setRtPriority(std::int32_t priority) noexcept
{
    m_RtPriority = clampRtPriority(priority);
}

std::optional<SchedPolicySetting> ProcessSchedPolicyDialog::setting() const noexcept
{
    if (!m_Choice.has_value())
    {
        return std::nullopt;
    }
    return SchedPolicySetting{.policy = *m_Choice, .rtPriority = Platform::isRealtimeSchedPolicy(*m_Choice) ? m_RtPriority : 0};
}

std::optional<SchedPolicySetting> ProcessSchedPolicyDialog::render()
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
    std::optional<SchedPolicySetting> applied;
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        return applied;
    }
    UI::Widgets::keepCurrentWindowInViewport();
    if (std::exchange(m_DismissRequested, false))
    {
        m_Open = false;
        m_Confirming = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return applied;
    }
    // Escape cancels, as Cancel does, from the choices or the confirm step: the policy is left alone (#1944).
    const bool escaped = UI::Widgets::dialogEscapePressed();
    // The confirm step for real time (#3219), or the choices; then the footer, whose buttons follow the step.
    const std::optional<SchedPolicySetting> chosenSetting = setting();
    const bool confirming = m_Confirming && chosenSetting.has_value();
    if (confirming)
    {
        renderConfirmBody(*chosenSetting);
    }
    else
    {
        renderChoices();
    }
    ImGui::Spacing();
    applied = renderFooter(escaped, confirming);
    ImGui::EndPopup();
    return applied;
}

std::optional<SchedPolicySetting> ProcessSchedPolicyDialog::renderFooter(bool escaped, bool confirming)
{
    // At the choices: Apply (disabled until a policy is chosen) returns the setting and closes, except that real time
    // goes on to the confirm step; Cancel closes. At the confirm step (@p confirming): Set returns the setting, Back
    // returns to the choices. Escape pressed this frame (@p escaped) closes from either, as Cancel does.
    std::optional<SchedPolicySetting> applied;
    const auto& theme = UI::Theme::get();
    const bool chosen = canApply();
    // Set at the real-time confirm step is drawn in the danger colour, as Kill's and End process tree's confirms are:
    // it is the consequential choice there, not the safe default (#3219).
    const UI::Widgets::ButtonFills applyFills = confirming ? UI::Widgets::ButtonFills{
                                                                 .resting = theme.scheme().dangerButton,
                                                                 .hovered = theme.scheme().dangerButtonHovered,
                                                                 .pressed = theme.scheme().dangerButtonActive,
                                                             }
                                                           : UI::Widgets::ButtonFills{
                                                                 .resting = theme.scheme().successButton,
                                                                 .hovered = theme.scheme().successButtonHovered,
                                                                 .pressed = theme.scheme().successButtonActive,
                                                             };
    const UI::Widgets::DialogFooterButton applyButton{
        .label = confirming ? SET_LABEL : APPLY_LABEL,
        .fills = &applyFills,
        .tooltip = chosen ? nullptr : "Choose a policy",
        .disabled = !chosen,
    };
    const UI::Widgets::DialogFooterButton cancelButton{
        .label = confirming ? BACK_LABEL : CANCEL_LABEL,
        .fills = nullptr,
        .tooltip = nullptr,
    };
    UI::Widgets::DialogFooterAction pressed = UI::Widgets::dialogFooter(
        applyButton, cancelButton, UI::Widgets::footerButtonWidth({APPLY_LABEL, CANCEL_LABEL, SET_LABEL, BACK_LABEL}, BUTTON_MIN_EM));
    if (escaped)
    {
        pressed = UI::Widgets::DialogFooterAction::Secondary;
    }
    switch (pressed)
    {
    case UI::Widgets::DialogFooterAction::Primary:
        // Read again: a radio button or the slider drawn above may have changed it this frame.
        if (const std::optional<SchedPolicySetting> now = setting(); now.has_value())
        {
            // Real time asks once more, naming what it is about to do (#3219); the others apply at once.
            if (Platform::isRealtimeSchedPolicy(now->policy) && !confirming)
            {
                m_Confirming = true;
                break;
            }
            applied = now;
            m_Open = false;
            m_Confirming = false;
            ImGui::CloseCurrentPopup();
        }
        break;
    case UI::Widgets::DialogFooterAction::Secondary:
        if (confirming && !escaped)
        {
            m_Confirming = false; // Back: to the choices, as they were
            break;
        }
        m_Open = false;
        m_Confirming = false;
        ImGui::CloseCurrentPopup();
        break;
    case UI::Widgets::DialogFooterAction::None:
    case UI::Widgets::DialogFooterAction::Leading:
        break;
    }
    return applied;
}

void ProcessSchedPolicyDialog::renderChoices()
{
    // What the dialog changes, a warning when the process's policy isn't offered (Deadline) or its real-time priority
    // wasn't read, one radio button per offered policy with what it does (real time disabled for TaskSmack itself),
    // then the real-time controls while FIFO or RR is chosen.
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
    if (m_CurrentRtPriorityUnread)
    {
        ImGui::TextColored(theme.scheme().textWarning,
                           ICON_FA_TRIANGLE_EXCLAMATION "  It runs under %s now, at a real-time priority TaskSmack couldn't read. Choose a "
                                                        "policy, and for real time a priority, below.",
                           m_Current == Platform::SchedPolicy::Fifo ? "FIFO" : "RR");
    }
    ImGui::Spacing();
    for (const SchedPolicyChoice& option : SCHED_POLICY_CHOICES)
    {
        const bool realTime = Platform::isRealtimeSchedPolicy(option.policy);
        ImGui::BeginDisabled(realTime && !offersRealtime());
        if (ImGui::RadioButton(option.label, m_Choice == option.policy))
        {
            choose(option.policy);
        }
        ImGui::EndDisabled();
        if (realTime && !offersRealtime())
        {
            ImGui::SetItemTooltip("%s", OWN_PROCESS_NO_REALTIME);
        }
        ImGui::Indent();
        ImGui::TextColored(theme.scheme().textMuted, "%s", option.description);
        ImGui::Unindent();
    }
    if (!offersRealtime())
    {
        ImGui::TextColored(theme.scheme().textMuted, "%s", OWN_PROCESS_NO_REALTIME);
    }
    if (warnsOffRealtime())
    {
        // RealtimeThreads::Change takes every real-time thread off real time: said before Apply (#3463 review).
        ImGui::TextColored(theme.scheme().textWarning,
                           ICON_FA_TRIANGLE_EXCLAMATION "  It runs under %s now. Applying this takes every thread off real time.",
                           m_Current == Platform::SchedPolicy::Fifo ? "FIFO" : "RR");
    }
    if (showsRealtimeWarning())
    {
        renderRealtimeControls();
    }
    ImGui::PopTextWrapPos();
}

void ProcessSchedPolicyDialog::renderRealtimeControls()
{
    // While FIFO or RR is chosen (#3219): the priority slider, then what real time risks, and, without CAP_SYS_NICE or
    // the helper, that it may be refused.
    const auto& theme = UI::Theme::get();
    ImGui::Spacing();
    ImGui::TextUnformatted("Real-time priority (1 lowest, 99 highest)");
    ImGui::SetNextItemWidth(-1.0F);
    int priority = m_RtPriority;
    if (ImGui::SliderInt(PRIORITY_SLIDER_ID,
                         &priority,
                         Platform::MIN_REALTIME_SCHED_PRIORITY,
                         Platform::MAX_REALTIME_SCHED_PRIORITY,
                         "%d",
                         ImGuiSliderFlags_AlwaysClamp))
    {
        setRtPriority(priority);
    }
    ImGui::Spacing();
    ImGui::TextColored(theme.scheme().textWarning, ICON_FA_TRIANGLE_EXCLAMATION "  Real time can make the desktop unresponsive");
    ImGui::TextUnformatted(REALTIME_WARNING);
    if (!m_RealtimeNeedsNoPassword)
    {
        ImGui::TextColored(theme.scheme().textMuted, "%s", REALTIME_MAY_BE_REFUSED);
    }
}

void ProcessSchedPolicyDialog::renderConfirmBody(const SchedPolicySetting& chosen) const
{
    // Names what Set will do: the process, its PID, the policy and its priority, what real time risks, and the
    // may-be-refused note where real time needs a password.
    const auto& theme = UI::Theme::get();
    const auto text = Platform::schedPolicyText(chosen.policy, static_cast<std::uint32_t>(chosen.rtPriority));
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextColored(theme.scheme().textWarning, ICON_FA_TRIANGLE_EXCLAMATION "  Run this process real time?");
    const std::string question =
        std::format("Set {} (PID {}) to {}? That is the real-time {} policy at priority {} (1 lowest, 99 highest).",
                    m_Name,
                    m_Target.pid,
                    text.view(),
                    chosen.policy == Platform::SchedPolicy::Fifo ? "FIFO" : "round robin",
                    chosen.rtPriority);
    ImGui::TextUnformatted(question.c_str());
    ImGui::TextUnformatted("Every thread runs ahead of every normal process until it blocks or yields. If it spins, the desktop may "
                           "stop responding until RT throttling lets it through; Back returns to the choices.");
    if (!m_RealtimeNeedsNoPassword)
    {
        ImGui::TextColored(theme.scheme().textMuted, "%s", REALTIME_MAY_BE_REFUSED);
    }
    ImGui::PopTextWrapPos();
}

Detail::ActionResultMessage schedPolicyResultMessage(const std::string& name,
                                                     std::int32_t pid,
                                                     const SchedPolicySetting& setting,
                                                     const Platform::ProcessActionResult& result)
{
    if (!result.success)
    {
        return {.ok = false, .text = result.errorMessage};
    }
    std::string text = std::format("Scheduling policy of {} (PID {}) set to {}",
                                   name,
                                   pid,
                                   Platform::schedPolicyText(setting.policy, static_cast<std::uint32_t>(setting.rtPriority)).view());
    if (!result.successNote.empty())
    {
        // Threads that kept a real-time policy the dialog didn't show (a real-time worker under a Normal main thread)
        text += "; ";
        text += result.successNote;
    }
    return {.ok = true, .text = std::move(text)};
}

} // namespace App
