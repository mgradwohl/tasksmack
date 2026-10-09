#include "ProcessIoPriorityView.h"

#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "ProcessPriorityView.h"
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

constexpr const char* APPLY_LABEL = "Apply";
constexpr const char* RESET_LABEL = "Reset to default";

} // namespace

// Renders the I/O priority section under the nice control: the row label (the current value on its
// tooltip), one slider through every class and level (#1540), "Reset to default" and Apply under it, and
// the error lines. The caller has checked canSetIoPriority.
void ProcessIoPriorityView::render(Platform::IProcessActions* actions,
                                   std::optional<std::int32_t> currentNice,
                                   const Platform::ProcessTarget& target,
                                   bool realtimeSettable)
{
    // An edit made for another process is never shown for, or applied to, this one.
    (void) dropEditIfTargetMoved(target);
    (void) refreshCurrent(actions, target, ImGui::GetTime());
    syncToProcess();

    // Its own ID scope: the nice control above has an "Apply" button too.
    ImGui::PushID("io_priority");

    const std::string currentDetail =
        m_Current.has_value() ? "current: " + Detail::describeIoPriority(*m_Current, currentNice) : std::string("current: unknown");
    ImGui::TextUnformatted("I/O priority");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", currentDetail.c_str());
    }

    // One slider for class and level, highest priority at the left like the nice slider. Without the
    // privilege Realtime needs it starts at Best-effort 0, and a process already in Realtime shows beyond
    // its start, hollow. A class never set shows hollow at the level nice derives; picking any other stop
    // makes it explicit.
    const Detail::DiscretePrioritySlider& slider = realtimeSettable ? Detail::IO_PRIORITY_SLIDER : Detail::IO_PRIORITY_SLIDER_NO_REALTIME;
    const Detail::IoStop shown = Detail::ioStopFor(m_Edit, currentNice.value_or(Domain::Priority::NORMAL_NICE), realtimeSettable);
    const Detail::DiscretePick pick = Detail::renderDiscretePrioritySlider(slider, shown.index, shown.inherited);
    if (pick.index != shown.index && pick.index >= 0)
    {
        editIoPriority(Detail::ioPriorityForStop(pick.index, realtimeSettable), target);
    }

    // [Reset to default] [Apply], right-aligned under the slider. Reset is there only while the value
    // shown is explicit: back to IOPRIO_CLASS_NONE, which follows the nice value.
    const auto& theme = UI::Theme::get();
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool enabled = canApply(target);
    const bool waiting = waitingForProcessDetails(target);
    const bool showReset = m_Edit.ioClass != Platform::IoPriorityClass::None;
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
            ImGui::SetTooltip("Follow the nice value again (best-effort, at the level it derives); Apply sets it");
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
        ImGui::SetTooltip("%s", waiting ? "Waiting for process details" : "Apply the selected I/O priority to the process");
    }
    (void) UI::Widgets::trailingNote(currentDetail);

    // Wrapped at the content edge: the privilege and identity messages are long, and the panel does
    // not scroll horizontally, so unwrapped they would lose the remediation at narrow widths.
    if (!m_Error.empty())
    {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextColored(theme.scheme().textError, ICON_FA_CIRCLE_EXCLAMATION "  %s", m_Error.c_str());
        ImGui::PopTextWrapPos();
    }
    else if (!m_ReadError.empty())
    {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0F);
        ImGui::TextColored(theme.scheme().textMuted, ICON_FA_CIRCLE_INFO "  %s", m_ReadError.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PopID();
}

} // namespace App
