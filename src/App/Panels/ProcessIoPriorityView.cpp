#include "ProcessIoPriorityView.h"

#include "AuthorizeWording.h"
#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"
#include "Platform/IoPriorityTypes.h"
#include "Platform/SchedPolicy.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "ProcessPriorityView.h"
#include "RestartAsAdministratorButton.h"
#include "UI/ChromeWidgets.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace App
{

namespace
{

constexpr const char* APPLY_LABEL = ICON_FA_CHECK "  Apply"; // As the nice control's Apply above it (#2005)
constexpr const char* RESET_LABEL = "Reset to default";

} // namespace

// Renders the I/O priority section under the nice control: the row label (the current value on its
// tooltip), one slider through every class and level (#1540), "Reset to default" and Apply under it, and
// the error lines. The caller has checked canSetIoPriority.
void ProcessIoPriorityView::render(Platform::IProcessActions* actions,
                                   std::optional<std::int32_t> currentNice,
                                   const Platform::ProcessTarget& target,
                                   bool realtimeSettable,
                                   bool windowsHints,
                                   Platform::SchedPolicy schedPolicy)
{
    // An apply finished on its worker since the last frame (#2305): its result first, so the read
    // below sees the read it asks for.
    update();
    // An edit made for another process is never shown for, or applied to, this one.
    (void) dropEditIfTargetMoved(target);
    // The current value is read off the UI thread by updateRead(), from the panel's update path (#2734).
    markDrawn();
    syncToProcess();
    // Windows (#3362): another user's or an elevated process often refuses the read, but the helper can make the
    // identity-checked set, so Apply goes ahead with the current value unknown. Cheap: reports what was found at
    // construction (IProcessActions::actionCapabilities()).
    allowApplyWhenReadDenied(windowsHints && actions != nullptr && actions->actionCapabilities().canAuthorizeInteractively);

    // Its own ID scope: the nice control above has an "Apply" button too.
    ImGui::PushID("io_priority");

    const std::string currentDetail = m_Current.has_value() ? "current: " + Detail::describeIoPriority(*m_Current, currentNice, schedPolicy)
                                                            : std::string("current: unknown");
    ImGui::TextUnformatted("I/O priority");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", currentDetail.c_str());
    }

    // One slider for class and level, highest priority at the left like the nice slider. Without the
    // privilege Realtime needs it starts at Best-effort 0, and a process already in Realtime shows beyond
    // its start, hollow. A class never set shows hollow at the level nice derives; picking any other stop
    // makes it explicit. Windows (#3362) has its four hints instead, Critical shown beyond the start.
    const Detail::DiscretePrioritySlider& linuxSlider =
        realtimeSettable ? Detail::IO_PRIORITY_SLIDER : Detail::IO_PRIORITY_SLIDER_NO_REALTIME;
    const Detail::DiscretePrioritySlider& slider = windowsHints ? Detail::IO_HINT_SLIDER : linuxSlider;
    const Detail::IoStop shown =
        windowsHints ? Detail::IoStop{.index = Detail::ioHintStopFor(m_Edit), .inherited = false}
                     : Detail::ioStopFor(m_Edit, currentNice.value_or(Domain::Priority::NORMAL_NICE), realtimeSettable, schedPolicy);
    const Detail::DiscretePick pick = Detail::renderDiscretePrioritySlider(slider, shown.index, shown.inherited);
    // On Windows an unknown value (a refused read) shows as Normal, a stand-in: choosing Normal itself is then an
    // edit to the Normal hint, so Apply can send it (#3362).
    const bool choseStandIn = windowsHints && pick.selected && m_Edit.ioClass != Platform::IoPriorityClass::WindowsHint;
    if ((pick.index != shown.index || choseStandIn) && pick.index >= 0)
    {
        editIoPriority(windowsHints ? Detail::ioHintForStop(pick.index) : Detail::ioPriorityForStop(pick.index, realtimeSettable), target);
    }

    // [Reset to default] [Apply], right-aligned under the slider. Reset is there only while the value
    // shown is explicit: back to IOPRIO_CLASS_NONE, which follows the nice value.
    const auto& theme = UI::Theme::get();
    const ImGuiStyle& style = ImGui::GetStyle();
    // Disabled, too, while an apply runs on its worker (#2305).
    const bool enabled = canApply(target) && !applying();
    const bool waiting = waitingForProcessDetails(target);
    // Windows has no "never set": Normal is the default, and a stop like any other.
    const bool showReset = !windowsHints && m_Edit.ioClass != Platform::IoPriorityClass::None;
    // The panel does not scroll horizontally: when the pair is wider than it, Reset takes a row of its
    // own above Apply, and neither is wider than the panel, so Apply is never clipped off its edge.
    const float available = std::max(ImGui::GetContentRegionAvail().x, 1.0F);
    const float applyWidth =
        std::min(ProcessDetailsLayout::computeActionButtonWidth(ImGui::CalcTextSize(APPLY_LABEL).x, style.FramePadding.x), available);
    const float resetWidth =
        showReset
            ? std::min(ProcessDetailsLayout::computeActionButtonWidth(ImGui::CalcTextSize(RESET_LABEL).x, style.FramePadding.x), available)
            : 0.0F;
    const float pairWidth = applyWidth + (showReset ? resetWidth + style.ItemSpacing.x : 0.0F);
    const bool stacked = showReset && pairWidth > available;
    const float rightEdge = std::min(pick.rightEdge, available);
    ImGui::Spacing();
    if (showReset)
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, rightEdge - (stacked ? resetWidth : pairWidth)));
        if (ImGui::Button(RESET_LABEL, ImVec2(resetWidth, 0.0F)))
        {
            editIoPriority({.ioClass = Platform::IoPriorityClass::None, .level = 0}, target);
        }
        if (ImGui::IsItemHovered())
        {
            // The class never set follows the CPU policy as well as nice (inheritedIoClass(), #3566's review).
            ImGui::SetTooltip("Follow the CPU scheduling policy and nice value again: best-effort at the level nice derives, or "
                              "realtime under a real-time policy, idle under SCHED_IDLE; Apply sets it");
        }
        if (stacked)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, rightEdge - applyWidth));
        }
        else
        {
            ImGui::SameLine();
        }
    }
    else
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, rightEdge - applyWidth));
    }
    if (!enabled)
    {
        ImGui::BeginDisabled();
    }
    if (UI::Widgets::filledButton(APPLY_LABEL,
                                  ImVec2(applyWidth, 0.0F),
                                  {
                                      .resting = theme.scheme().successButton,
                                      .hovered = theme.scheme().successButtonHovered,
                                      .pressed = theme.scheme().successButtonActive,
                                  },
                                  theme.scheme().textPrimary,
                                  theme.scheme().windowBg))
    {
        // The target is captured now, at the press, and checked against the edit's own.
        apply(actions, target);
    }
    if (!enabled)
    {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        const char* tooltip = "Apply the selected I/O priority to the process";
        if (applying())
        {
            tooltip = "Applying the last change";
        }
        else if (waiting)
        {
            tooltip = "Waiting for process details";
        }
        ImGui::SetTooltip("%s", tooltip);
    }
    (void) UI::Widgets::trailingNote(currentDetail);

    // Wrapped at the content edge: the privilege and identity messages are long, and the panel does
    // not scroll horizontally, so unwrapped they would lose the remediation at narrow widths.
    if (applying())
    {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", applyingText().c_str());
    }
    else if (!m_Error.empty())
    {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextColored(theme.scheme().textError, ICON_FA_CIRCLE_EXCLAMATION "  %s", m_Error.c_str());
        ImGui::PopTextWrapPos();
        // A permission refusal an administrator could redo (#2308): the lock and the words, never colour alone.
        if (offersAuthenticateAndRetry())
        {
            if (ImGui::SmallButton(AuthorizeWording::RETRY_LABEL))
            {
                retryAuthorized();
            }
            ImGui::SetItemTooltip("%s", AuthorizeWording::IO_PRIORITY_TOOLTIP);
        }
        else if (offersRestartAsAdministrator())
        {
            static_cast<void>(RestartAsAdministrator::drawButton()); // no helper here: the restart instead (#3669)
        }
    }
    else if (!m_ReadError.empty())
    {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextColored(theme.scheme().textMuted, ICON_FA_CIRCLE_INFO "  %s", m_ReadError.c_str());
        ImGui::PopTextWrapPos();
        // A refused read with no helper leaves Apply disabled: the restart is the way on (#3669).
        if (readRefusalOffersRestart(actions != nullptr && actions->actionCapabilities().canAuthorizeInteractively))
        {
            static_cast<void>(RestartAsAdministrator::drawButton());
        }
    }
    ImGui::PopID();
}

} // namespace App
