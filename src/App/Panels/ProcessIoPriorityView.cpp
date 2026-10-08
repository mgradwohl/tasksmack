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
                                         "  Realtime: served before everything else; needs CAP_SYS_NICE (or root)\n\n"
                                         "Changing another user's process needs CAP_SYS_NICE (or root).";

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

    const std::string currentDetail =
        m_Current.has_value() ? "current: " + Detail::describeIoPriority(*m_Current, currentNice) : std::string("current: unknown");
    // A row label, level with the controls, rather than a header: the Actions block's one-line
    // "I/O priority [class] [level] [Apply]" row (#1493). The current value follows Apply when it fits,
    // and is on the label's tooltip always.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("I/O priority");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", currentDetail.c_str());
    }
    ImGui::SameLine();

    // Apply on the same row, after the controls, so the section stays one line tall -- unless the panel
    // is too narrow: it does not scroll horizontally, so the row shrinks, then wraps, rather than clip.
    const auto& theme = UI::Theme::get();
    const float emPx = ImGui::GetFontSize();
    const bool enabled = canApply(target);
    const bool waiting = waitingForProcessDetails(target);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const Detail::IoPriorityRowLayout layout = Detail::computeIoPriorityRowLayout(
        ImGui::GetContentRegionAvail().x,
        emPx,
        spacing,
        UI::DialogMetrics::computeActionButtonWidth(ImGui::CalcTextSize("Apply").x, emPx, Detail::PRIORITY_APPLY_BUTTON_MIN_EM),
        Detail::ioClassHasLevels(m_Edit.ioClass));

    renderControls(currentNice, target, layout);
    if (!layout.applyOnNewLine)
    {
        ImGui::SameLine(0.0F, spacing);
    }
    if (!enabled)
    {
        ImGui::BeginDisabled();
    }
    if (UI::Widgets::filledButton("Apply",
                                  ImVec2(layout.applyWidth, 0.0F),
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
    if (!layout.applyOnNewLine)
    {
        (void) UI::Widgets::trailingNote(currentDetail);
    }

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

void ProcessIoPriorityView::renderControls(std::optional<std::int32_t> currentNice,
                                           const Platform::ProcessTarget& target,
                                           const Detail::IoPriorityRowLayout& layout)
{
    // The class combo.
    const std::string selectedName{Detail::ioPriorityClassName(m_Edit.ioClass)};
    ImGui::SetNextItemWidth(layout.comboWidth);
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

    // The level slider, only for the classes that have levels. The layout was made before the combo, so
    // a class picked this frame gets its slider next frame, when the row has room laid out for it.
    if (Detail::ioClassHasLevels(m_Edit.ioClass) && layout.sliderWidth > 0.0F)
    {
        if (!layout.sliderOnNewLine)
        {
            ImGui::SameLine();
        }
        int level = m_Edit.level;
        ImGui::SetNextItemWidth(layout.sliderWidth);
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
