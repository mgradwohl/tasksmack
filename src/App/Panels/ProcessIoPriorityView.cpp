#include "ProcessIoPriorityView.h"

#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "UI/ChromeWidgets.h"
#include "UI/DialogMetrics.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>

namespace App
{

namespace
{

/// The combo's tooltip: what each class means, briefly.
constexpr const char* IO_CLASS_TOOLTIP = "I/O scheduling class (ionice):\n"
                                         "  Default: best-effort, at a level derived from the nice value\n"
                                         "  Best-effort: shares the disk by level, 0 (highest) to 7 (lowest)\n"
                                         "  Idle: gets the disk only when no other process wants it\n"
                                         "  Realtime: served before everything else; needs root (CAP_SYS_ADMIN)\n\n"
                                         "Changing another user's process needs root.";

} // namespace

// Renders the I/O priority section under the nice control: a header with the current class and level,
// one row with the class combo, the level slider (Best-effort and Realtime) and Apply, and the error
// lines. The caller has checked canSetIoPriority.
void ProcessIoPriorityView::render(Platform::IProcessActions* actions,
                                   std::optional<std::int32_t> currentNice,
                                   const Platform::ProcessTarget& target)
{
    // An edit made for another process is never shown for, or applied to, this one.
    (void) dropEditIfTargetMoved(target);
    (void) refreshCurrent(actions, target, ImGui::GetTime());
    syncToProcess();

    // Its own ID scope: the nice control above has an "Apply" button too.
    ImGui::PushID("io_priority");
    ImGui::Spacing();
    ImGui::Spacing();

    const std::string currentDetail =
        m_Current.has_value() ? "current: " + Detail::describeIoPriority(*m_Current, currentNice) : std::string("current: unknown");
    (void) UI::Widgets::sectionHeader(ICON_FA_HARD_DRIVE, "I/O Priority", currentDetail);
    ImGui::Spacing();

    renderControls(currentNice, target);

    // Apply on the same row, after the controls: the section stays one line tall.
    const auto& theme = UI::Theme::get();
    const float emPx = ImGui::GetFontSize();
    const bool enabled = canApply(target);
    const bool waiting = waitingForProcessDetails(target);
    const float applyButtonWidth =
        UI::DialogMetrics::computeActionButtonWidth(ImGui::CalcTextSize("Apply").x, emPx, Detail::PRIORITY_APPLY_BUTTON_MIN_EM);
    ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x);
    if (!enabled)
    {
        ImGui::BeginDisabled();
    }
    if (UI::Widgets::filledButton("Apply",
                                  ImVec2(applyButtonWidth, 0.0F),
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

    if (!m_Error.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(theme.scheme().textError, ICON_FA_CIRCLE_EXCLAMATION "  %s", m_Error.c_str());
    }
    else if (!m_ReadError.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(theme.scheme().textMuted, ICON_FA_CIRCLE_INFO "  %s", m_ReadError.c_str());
    }
    ImGui::PopID();
}

void ProcessIoPriorityView::renderControls(std::optional<std::int32_t> currentNice, const Platform::ProcessTarget& target)
{
    const float emPx = ImGui::GetFontSize();

    // The class combo.
    const std::string selectedName{Detail::ioPriorityClassName(m_Edit.ioClass)};
    ImGui::SetNextItemWidth(Detail::IO_PRIORITY_CLASS_COMBO_WIDTH_EM * emPx);
    if (ImGui::BeginCombo("##io_class", selectedName.c_str()))
    {
        for (const Platform::IoPriorityClass ioClass : Detail::SETTABLE_IO_PRIORITY_CLASSES)
        {
            const std::string optionName{Detail::ioPriorityClassName(ioClass)};
            const bool isSelected = ioClass == m_Edit.ioClass;
            if (ImGui::Selectable(optionName.c_str(), isSelected) && !isSelected)
            {
                editClass(ioClass, currentNice, target);
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", IO_CLASS_TOOLTIP);
    }

    // The level slider, only for the classes that have levels.
    if (Detail::ioClassHasLevels(m_Edit.ioClass))
    {
        ImGui::SameLine();
        int level = m_Edit.level;
        ImGui::SetNextItemWidth(Detail::IO_PRIORITY_LEVEL_SLIDER_WIDTH_EM * emPx);
        if (ImGui::SliderInt("##io_level",
                             &level,
                             Domain::Priority::MIN_IO_LEVEL,
                             Domain::Priority::MAX_IO_LEVEL,
                             "level %d",
                             ImGuiSliderFlags_AlwaysClamp))
        {
            editIoPriority({.ioClass = m_Edit.ioClass, .level = level}, target);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Level within the class: 0 is the highest priority, 7 the lowest.");
        }
    }
}

} // namespace App
