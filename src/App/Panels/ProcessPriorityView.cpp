#include "ProcessPriorityView.h"

#include "Platform/IProcessActions.h"
#include "ProcessDetailsLayout.h"
#include "ProcessDetailsPanel_PriorityHelpers.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#ifndef _WIN32
#include "Domain/PriorityConfig.h" // NORMAL_NICE for the nice slider's 0 key; the Windows class combo has no slider
#endif

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace App
{

namespace
{

// The Apply button's label, named once so its width is measured from what is drawn (icon included).
constexpr const char* APPLY_LABEL = ICON_FA_CHECK "  Apply";

#ifndef _WIN32
using Detail::getNiceColor;
using Detail::getNiceFromPosition;
using Detail::getNicePosition;
using Detail::NICE_MAX;
using Detail::NICE_MIN;
using Detail::NICE_RANGE;
using Detail::PRIORITY_GRADIENT_SEGMENTS;

/// Captures all computed layout values in one place for the slider's drawing helpers
struct PrioritySliderContext
{
    ImDrawList* drawList = nullptr;
    ImVec2 cursorStart;         // Screen position where badge area starts
    ImVec2 sliderMin;           // Top-left of slider bar (screen coords)
    ImVec2 sliderMax;           // Bottom-right of slider bar (screen coords)
    float sliderLocalX = 0.0F;  // Slider X position in window-local coords (for cursor positioning)
    float normalizedPos = 0.0F; // 0.0 = nice -20, 1.0 = nice 19
    int32_t niceValue = 0;      // Current nice value
    const ImGuiStyle* style = nullptr;
    ImVec4 priorityHighColor;              // Theme color for high-priority end
    ImVec4 priorityNormalColor;            // Theme color for normal priority
    ImVec4 priorityLowColor;               // Theme color for low-priority end
    Detail::PrioritySliderMetrics metrics; // Font-derived pixel geometry for this frame
};

void drawPriorityBadge(ImDrawList* drawList, const PrioritySliderContext& ctx)
{
    const float badgeX = ctx.cursorStart.x + (ctx.normalizedPos * ctx.metrics.sliderWidth);
    const float badgeY = ctx.cursorStart.y;

    // Badge text
    const std::string valueText = std::to_string(ctx.niceValue);
    const ImVec2 textSize = ImGui::CalcTextSize(valueText.c_str());
    const float badgeWidth = textSize.x + (ctx.style->FramePadding.x * 2.0F);
    const float badgeHalfWidth = badgeWidth * 0.5F;

    // Keep the badge over the slider rather than hanging off either end
    const float clampedBadgeX = Detail::computeBadgeCenterX(badgeX, ctx.cursorStart.x, ctx.metrics.sliderWidth, badgeHalfWidth);

    // Badge rectangle
    const ImVec2 badgeMin(clampedBadgeX - badgeHalfWidth, badgeY);
    const ImVec2 badgeMax(clampedBadgeX + badgeHalfWidth, badgeY + ctx.metrics.badgeHeight);

    // Badge color based on nice value
    const ImU32 badgeColorU32 = getNiceColor(ctx.niceValue, ctx.priorityHighColor, ctx.priorityNormalColor, ctx.priorityLowColor);

    // Only the top corners are rounded: the arrow hangs from the bottom edge, which must be flat for
    // the arrow to join it rather than dangle from a curve (#1533).
    drawList->AddRectFilled(badgeMin, badgeMax, badgeColorU32, ctx.metrics.badgeCornerRadius, ImDrawFlags_RoundCornersTop);

    // Arrow pointing down from the badge at the thumb, its base held to the badge's bottom edge so it
    // never overhangs where the badge is clamped at a track end.
    const Detail::BadgeArrowBase arrowBase = Detail::computeBadgeArrowBase(badgeX, ctx.metrics.badgeArrowSize, badgeMin.x, badgeMax.x);
    const ImVec2 arrowTip(badgeX, badgeMax.y + ctx.metrics.badgeArrowSize);
    const ImVec2 arrowLeft(arrowBase.left, badgeMax.y);
    const ImVec2 arrowRight(arrowBase.right, badgeMax.y);
    drawList->AddTriangleFilled(arrowLeft, arrowRight, arrowTip, badgeColorU32);

    // The theme's badge text colour when it reaches 4.5:1 on this badge's fill, else its window
    // background when that does, else black or white (badgeTextFor): a fixed colour was unreadable on
    // the nice-0 badge in most dark themes (#1130).
    const UI::ColorScheme& scheme = UI::Theme::get().scheme();
    const ImU32 badgeTextColorU32 = ImGui::ColorConvertFloat4ToU32(
        Detail::badgeTextFor(Detail::unpackColor(badgeColorU32), scheme.priorityBadgeTextColor, scheme.windowBg));

    // Draw badge text
    const ImVec2 textPos(clampedBadgeX - (textSize.x * 0.5F), badgeY + ((ctx.metrics.badgeHeight - textSize.y) * 0.5F));
    drawList->AddText(textPos, badgeTextColorU32, valueText.c_str());
}

void drawPriorityGradient(ImDrawList* drawList, const PrioritySliderContext& ctx)
{
    constexpr auto SEGMENTS = static_cast<int>(PRIORITY_GRADIENT_SEGMENTS);
    const float segmentWidth = ctx.metrics.sliderWidth / PRIORITY_GRADIENT_SEGMENTS;

    for (int i = 0; i < SEGMENTS; ++i)
    {
        const float t1 = static_cast<float>(i) / PRIORITY_GRADIENT_SEGMENTS;
        const float t2 = static_cast<float>(i + 1) / PRIORITY_GRADIENT_SEGMENTS;
        const int nice1 = NICE_MIN + static_cast<int>(t1 * static_cast<float>(NICE_RANGE));
        const int nice2 = NICE_MIN + static_cast<int>(t2 * static_cast<float>(NICE_RANGE));
        const ImU32 col1 = getNiceColor(nice1, ctx.priorityHighColor, ctx.priorityNormalColor, ctx.priorityLowColor);
        const ImU32 col2 = getNiceColor(nice2, ctx.priorityHighColor, ctx.priorityNormalColor, ctx.priorityLowColor);

        const ImVec2 segMin(ctx.sliderMin.x + (static_cast<float>(i) * segmentWidth), ctx.sliderMin.y);
        const ImVec2 segMax(ctx.sliderMin.x + (static_cast<float>(i + 1) * segmentWidth), ctx.sliderMax.y);

        drawList->AddRectFilledMultiColor(segMin, segMax, col1, col2, col2, col1);
    }
}

void drawPriorityThumb(ImDrawList* drawList, const PrioritySliderContext& ctx)
{
    const float thumbX = ctx.sliderMin.x + (ctx.normalizedPos * ctx.metrics.sliderWidth);
    const float thumbRadius = ctx.metrics.thumbRadius;
    const ImVec2 thumbCenter(thumbX, ctx.sliderMin.y + (ctx.metrics.sliderHeight * 0.5F));

    // The thumb sits on the track at the current nice value, which is the badge's fill, so it takes the
    // badge text's colour: readable there by construction rather than a fixed colour that vanished into
    // the light green middle of the track on dark themes (#1130).
    const UI::ColorScheme& scheme = UI::Theme::get().scheme();
    const ImU32 trackColorU32 = getNiceColor(ctx.niceValue, ctx.priorityHighColor, ctx.priorityNormalColor, ctx.priorityLowColor);
    const ImU32 thumbFillColorU32 = ImGui::ColorConvertFloat4ToU32(
        Detail::badgeTextFor(Detail::unpackColor(trackColorU32), scheme.priorityBadgeTextColor, scheme.windowBg));

    // Thumb outline
    drawList->AddCircleFilled(thumbCenter, thumbRadius + ctx.metrics.thumbOutlineThickness, ImGui::GetColorU32(ImGuiCol_Border));
    // Thumb fill
    drawList->AddCircleFilled(thumbCenter, thumbRadius, thumbFillColorU32);
}

/// The nice value the slider's input picks this frame, starting from @p current: dragging sets it from
/// the pointer's position on the track, and while the slider is focused,
/// Left/Right (±1), PgUp/PgDown (±5), Home/End (min/max) and 0 (default) move it.
[[nodiscard]] int32_t sliderInputNice(const PrioritySliderContext& ctx, int32_t current)
{
    // Mouse input: drag to set value
    if (ImGui::IsItemActive())
    {
        const float mouseX = ImGui::GetIO().MousePos.x;
        const float relX = std::clamp((mouseX - ctx.sliderMin.x) / ctx.metrics.sliderWidth, 0.0F, 1.0F);
        current = getNiceFromPosition(relX);
    }

    // Keyboard input: adjust value when focused
    if (!ImGui::IsItemFocused())
    {
        return current;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
    {
        return Detail::stepNice(current, -1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
    {
        return Detail::stepNice(current, 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
    {
        return Detail::stepNice(current, -5); // Page Up = higher priority = lower nice value
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
    {
        return Detail::stepNice(current, 5); // Page Down = lower priority = higher nice value
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home))
    {
        return NICE_MIN; // Highest priority (-20)
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End))
    {
        return NICE_MAX; // Lowest priority (19)
    }
    if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0))
    {
        return Domain::Priority::NORMAL_NICE; // Default priority
    }
    return current;
}

void drawPriorityScaleLabels(const PrioritySliderContext& ctx)
{
    const auto& theme = UI::Theme::get();

    // The track is sized to leave exactly "Low"'s width before the edge, so a wrap position pushed by
    // an enclosing container (the Actions block wraps its result lines, #1511) would split it into
    // "Lo" / "w" (#1560). The scale labels never wrap.
    ImGui::PushTextWrapPos(-1.0F);

    // "Low" label (right of slider, colored blue)
    // Position it after the slider with padding (sliderLocalX + width = right edge)
    const float lowLabelX = ctx.sliderLocalX + ctx.metrics.sliderWidth + ctx.metrics.labelPadding;
    ImGui::SameLine();
    ImGui::SetCursorPosX(lowLabelX);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textInfo);
    ImGui::TextUnformatted("Low");
    ImGui::PopStyleColor();

    // "Default" label centered below the 0 position on the slider
    // Use getNicePosition(0) for consistency with other position calculations
    const float defaultX = ctx.sliderLocalX + (getNicePosition(0) * ctx.metrics.sliderWidth);
    const ImVec2 defaultSize = ImGui::CalcTextSize("Default");
    ImGui::SetCursorPosX(defaultX - (defaultSize.x * 0.5F));
    ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textMuted);
    ImGui::TextUnformatted("Default");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}
#endif

} // namespace

// Renders the process-priority section: the current priority, the platform's control (the gradient
// slider, or the Windows priority-class combo), the Apply button and the error line; then, where the
// platform can set it (Linux), the I/O priority control (#803). No-op if the process actions support
// neither.
void ProcessPriorityView::render(Platform::IProcessActions* actions,
                                 const Platform::ProcessActionCapabilities& capabilities,
                                 std::optional<std::int32_t> currentNice,
                                 const Platform::ProcessTarget& target)
{
    if (!capabilities.canSetPriority && !capabilities.canSetIoPriority)
    {
        return;
    }

    // Rows of the Overview's Actions block (#1493), under its header: no separator or header of its own.
    if (capabilities.canSetPriority)
    {
        renderNiceControl(actions, currentNice, target);
    }
    // The I/O priority (#803) under the nice control, with its own edit and Apply; Linux only.
    if (capabilities.canSetIoPriority)
    {
        m_IoPriorityView.render(actions, currentNice, target);
    }
}

void ProcessPriorityView::renderNiceControl(Platform::IProcessActions* actions,
                                            std::optional<std::int32_t> currentNice,
                                            const Platform::ProcessTarget& target)
{
    // An edit made for another process is never shown for, or applied to, this one.
    (void) dropEditIfTargetMoved(target);

    // Initialize the control from the current process nice value if not changed
    syncToProcess(currentNice);

#ifdef _WIN32
    // One line (#1493): Priority [class] [Apply]. The current class is Runtime's Priority row, and
    // the label's tooltip, not repeated after Apply (#1537).
    (void) renderClassCombo(currentNice.value_or(0), target);
    ImGui::SameLine();
    renderApplyButton(actions, currentNice, target, 0.0F);
#else
    const float controlRightEdge = renderSlider(currentNice.value_or(0), target);
    ImGui::Spacing();
    renderApplyButton(actions, currentNice, target, controlRightEdge);
#endif

    // Display persistent error message if priority change failed
    if (!m_Error.empty())
    {
        const auto& theme = UI::Theme::get();
        ImGui::Spacing();
        ImGui::TextColored(theme.scheme().textError, ICON_FA_CIRCLE_EXCLAMATION "  %s", m_Error.c_str());
    }
}

#ifdef _WIN32
float ProcessPriorityView::renderClassCombo(std::int32_t currentNice, const Platform::ProcessTarget& target)
{
    // Windows has priority classes, not nice values (#1204): name the current class and offer the five
    // settable ones in a combo (Detail::renderPriorityPicker()). Each writes its representative nice
    // value through setPriority(), which maps it back to that class; Realtime can only be shown.
    const auto& theme = UI::Theme::get();
    const std::string currentClassName{Detail::windowsPriorityClassName(Detail::windowsPriorityClassFromNice(currentNice))};
    // A row label, level with the combo, rather than a header: the Actions block's row (#1493). The
    // current class is on its tooltip; Runtime's Priority row shows it too.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Priority");
    ImGui::SetItemTooltip("Current priority class: %s", currentClassName.c_str());
    ImGui::SameLine();

    const Detail::WindowsPriorityClass selectedClass = Detail::windowsPriorityClassFromNice(m_NiceValue);
    const Detail::PriorityPick pick = Detail::renderPriorityPicker(m_NiceValue);
    editNice(pick.nice, target);
    if (selectedClass == Detail::WindowsPriorityClass::Realtime)
    {
        ImGui::TextColored(theme.scheme().textWarning,
                           ICON_FA_TRIANGLE_EXCLAMATION "  Realtime was set outside TaskSmack; it can be lowered here, not set");
    }
    return pick.rightEdge;
}

namespace Detail
{

PriorityPick renderPriorityPicker(std::int32_t shown)
{
    const float emPx = ImGui::GetFontSize();
    PriorityPick pick{.nice = shown, .rightEdge = 0.0F};
    const WindowsPriorityClass selectedClass = windowsPriorityClassFromNice(shown);
    const std::string selectedClassName{windowsPriorityClassName(selectedClass)};
    const float comboWidth = std::min(PRIORITY_CLASS_COMBO_WIDTH_EM * emPx, std::max(ImGui::GetContentRegionAvail().x, 1.0F));
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##priority_class", selectedClassName.c_str()))
    {
        for (const WindowsPriorityClass priorityClass : SETTABLE_WINDOWS_PRIORITY_CLASSES)
        {
            const std::string optionName{windowsPriorityClassName(priorityClass)};
            const bool isSelected = priorityClass == selectedClass;
            if (ImGui::Selectable(optionName.c_str(), isSelected) && !isSelected)
            {
                pick.nice = windowsPriorityClassNice(priorityClass);
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
        ImGui::SetTooltip("Windows priority class: higher classes get CPU time first.\n"
                          "Realtime cannot be set here.\n\n"
                          "Note: Changing another user's or an elevated process typically requires administrator privileges");
    }
    pick.rightEdge = comboWidth;
    return pick;
}

} // namespace Detail
#else
float ProcessPriorityView::renderSlider(std::int32_t currentNice, const Platform::ProcessTarget& target)
{
    // A row label rather than a header: the Actions block's row (#1493). The current nice value is
    // Runtime's Priority row ("Normal (nice: 0)") and the label's tooltip, not repeated beside it
    // (#1537); the slider's badge shows the value picked.
    ImGui::TextUnformatted("Priority");
    ImGui::SetItemTooltip("Current nice value: %d", currentNice);

    const Detail::PriorityPick pick = Detail::renderPriorityPicker(m_NiceValue);
    editNice(pick.nice, target);
    return pick.rightEdge;
}

namespace Detail
{

PriorityPick renderPriorityPicker(std::int32_t shown)
{
    const auto& theme = UI::Theme::get();
    const float emPx = ImGui::GetFontSize();

    auto* drawList = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();

    // ========================================
    // Custom gradient priority slider (drawn by the helpers above)
    // Layout: High [====gradient====] Low
    //                   Default
    // ========================================

    // Calculate "High" label width for offsetting the slider
    const float labelPadding = Detail::PRIORITY_LABEL_PADDING_EM * emPx;
    const ImVec2 highLabelSize = ImGui::CalcTextSize("High");
    const float highLabelOffset = highLabelSize.x + labelPadding;

    // The track gets whatever the panel has left once both labels and their padding are placed, so a
    // large font on a narrow panel shortens the track rather than pushing "Low" out of view.
    const float availableTrackWidth = ImGui::GetContentRegionAvail().x - highLabelOffset - labelPadding - ImGui::CalcTextSize("Low").x;

    // Build context for helper methods
    PrioritySliderContext ctx;
    ctx.drawList = drawList;
    ctx.niceValue = shown;
    ctx.normalizedPos = getNicePosition(shown);
    ctx.style = &style;
    ctx.priorityHighColor = theme.scheme().priorityHighColor;
    ctx.priorityNormalColor = theme.scheme().priorityNormalColor;
    ctx.priorityLowColor = theme.scheme().priorityLowColor;
    // A panel too narrow to leave any room is not "unconstrained": pass the smallest positive width
    // so the track shrinks to that instead of taking its full authored width and being clipped.
    ctx.metrics = Detail::computePrioritySliderMetrics(emPx, std::max(availableTrackWidth, 1.0F));
    const Detail::PrioritySliderMetrics& metrics = ctx.metrics;

    // Reserve space for badge above slider (offset by High label width)
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    ctx.cursorStart = ImVec2(rowStart.x + highLabelOffset, rowStart.y);
    ImGui::Dummy(ImVec2(highLabelOffset + metrics.sliderWidth, metrics.badgeHeight + metrics.badgeArrowSize));

    // Draw the value badge/callout above the slider position
    drawPriorityBadge(drawList, ctx);

    // Draw "High" label (left of slider, vertically centered with slider)
    const float sliderRowY = ImGui::GetCursorPosY();
    const float labelCenterY = sliderRowY + ((metrics.sliderHeight - highLabelSize.y) * 0.5F);
    ImGui::SetCursorPosY(labelCenterY);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textError);
    ImGui::TextUnformatted("High");
    ImGui::PopStyleColor();

    // Position the slider after "High" label on same line
    ImGui::SameLine();
    ImGui::SetCursorPosY(sliderRowY);

    // Draw the gradient slider bar
    ctx.sliderMin = ImGui::GetCursorScreenPos();
    ctx.sliderMax = ImVec2(ctx.sliderMin.x + metrics.sliderWidth, ctx.sliderMin.y + metrics.sliderHeight);
    // Store window-local X coordinate for scale label positioning
    ctx.sliderLocalX = ctx.sliderMin.x - ImGui::GetWindowPos().x;

    // Draw gradient background (red -> green -> blue), the slider border, and the thumb
    drawPriorityGradient(drawList, ctx);
    drawList->AddRect(ctx.sliderMin, ctx.sliderMax, ImGui::GetColorU32(ImGuiCol_Border), metrics.sliderCornerRadius);
    drawPriorityThumb(drawList, ctx);

    // Make the slider interactive with an invisible button
    ImGui::InvisibleButton("##priority_slider", ImVec2(metrics.sliderWidth, metrics.sliderHeight));
    const std::int32_t picked = sliderInputNice(ctx, shown);

    // Draw "Low" label and "Default" label
    drawPriorityScaleLabels(ctx);

    // Tooltip on hover with keyboard shortcut hints
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Nice value: -20 (highest priority) to 19 (lowest priority)\n"
                          "Lower values = higher priority (more CPU time)\n"
                          "Normal priority = 0\n\n"
                          "Keyboard shortcuts:\n"
                          "  Left/Right: Adjust by 1\n"
                          "  PgUp/PgDown: Adjust by 5\n"
                          "  Home/End: Min/Max priority\n"
                          "  0: Reset to default\n\n"
                          "Note: Setting values below 0 typically requires root/admin privileges");
    }
    // The track starts after the "High" label, so the label offset belongs in the sum: without it the
    // Apply button stopped that far short of the track's right edge.
    return {.nice = picked, .rightEdge = highLabelOffset + metrics.sliderWidth};
}

} // namespace Detail
#endif

void ProcessPriorityView::renderApplyButton(Platform::IProcessActions* actions,
                                            std::optional<std::int32_t> currentNice,
                                            const Platform::ProcessTarget& target,
                                            float controlRightEdge)
{
    const auto& theme = UI::Theme::get();
    // Disabled without an edit, and also until a snapshot has confirmed the start time: no platform
    // acts on an unknown one (IProcessActions' checkProcessIdentity()).
    const bool enabled = canApply(currentNice, target);
    const bool waiting = waitingForProcessDetails(currentNice, target);

    // Right-align the Apply button. As wide as its label, the same rule as Terminate's and Kill's
    // (#1537): its old em floor made it wider than the class combo beside it.
    // Capped to the panel for the same reason the track is: the content area does not scroll
    // horizontally, so a button wider than the space available would be clipped.
    const float applyButtonWidth =
        std::min(ProcessDetailsLayout::computeActionButtonWidth(ImGui::CalcTextSize(APPLY_LABEL).x, ImGui::GetStyle().FramePadding.x),
                 std::max(ImGui::GetContentRegionAvail().x, 1.0F));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, controlRightEdge - applyButtonWidth));

    // Apply button with success (green) styling
    if (!enabled)
    {
        ImGui::BeginDisabled();
    }
    // As for the Settings dialog's Apply: the label is drawn in whichever of the theme's text
    // colour and window background reads better on the fill showing in the current state (#969).
    if (UI::Widgets::filledButton(APPLY_LABEL,
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
        apply(actions, target, currentNice);
    }
    if (!enabled)
    {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", waiting ? "Waiting for process details" : "Apply the selected priority to the process");
    }
}

} // namespace App
