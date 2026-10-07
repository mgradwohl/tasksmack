#pragma once

// The shared chrome every panel and dialog draws the same way (#1200): section headers, the gap
// between sections, and the dialog footer. The arithmetic is in UI/ChromeLayout.h, where it is
// tested without ImGui.

#include "UI/ChromeLayout.h"
#include "UI/DialogMetrics.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace UI::Widgets
{

/// A section header: the icon and title in the bold body font, then an optional quieter detail in
/// the muted colour, all on one line (#1200).
///
/// The headers used to be body-weight text with a debug counter appended -- "CPU Usage (300
/// samples)" -- so title, value and note all shared one weight and the eye had nowhere to land. The
/// title is now a weight apart from the text beneath it, at the same size, so a header row is as tall
/// as before and nothing laid out around it moves. A sample count belongs in @p sampleCount, which
/// shows on hover; it is never part of the title.
///
/// The header is one item (a group), so a caller can put something on its line with SameLine() --
/// every chart's value strip is placed after the item drawn just before it -- or attach its own
/// tooltip with the return value.
///
/// @param icon         An ICON_FA_* glyph, or nullptr for none.
/// @param title        What the section is: "CPU Usage", "Network Throughput".
/// @param detail       Quieter context shown after the title, e.g. which interface; empty for none.
/// @param sampleCount  How many samples the section's chart is drawn from, shown as a tooltip.
/// @return Whether the header is hovered, for a caller that adds its own tooltip.
inline bool
sectionHeader(const char* icon, std::string_view title, std::string_view detail = {}, std::optional<std::size_t> sampleCount = std::nullopt)
{
    const auto& theme = UI::Theme::get();
    std::array<char, 256> labelBuffer{};
    const std::string_view label =
        ChromeLayout::composeHeaderLabel(labelBuffer, (icon != nullptr) ? std::string_view{icon} : std::string_view{}, title);

    ImGui::BeginGroup();
    ImFont* bold = theme.boldFont();
    if (bold != nullptr)
    {
        ImGui::PushFont(bold);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary);
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    ImGui::PopStyleColor();
    if (bold != nullptr)
    {
        ImGui::PopFont();
    }
    if (!detail.empty())
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textMuted);
        ImGui::TextUnformatted(detail.data(), detail.data() + detail.size());
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();

    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    if (hovered && sampleCount.has_value())
    {
        std::array<char, 64> tooltipBuffer{};
        const std::string_view tooltip = ChromeLayout::formatSampleCount(tooltipBuffer, *sampleCount);
        if (ImGui::BeginTooltip())
        {
            ImGui::TextUnformatted(tooltip.data(), tooltip.data() + tooltip.size());
            ImGui::EndTooltip();
        }
    }
    return hovered;
}

/// The vertical gap between two sections of a dialog or pane (#1200): ChromeLayout's
/// SECTION_GAP_ITEM_SPACINGS item spacings, so it scales with the style. Use it where a new section
/// starts, in place of a run of Spacing() calls.
inline void sectionGap()
{
    ImGui::Dummy(ImVec2(0.0F, ChromeLayout::sectionGapItemHeight(ImGui::GetStyle().ItemSpacing.y)));
}

/// One button of a dialog footer.
struct DialogFooterButton
{
    const char* label = nullptr;        ///< Visible label and ID; nullptr: no button in this slot.
    const ButtonFills* fills = nullptr; ///< Its own fill (Save, a destructive confirm); nullptr: an ordinary button.
    const char* tooltip = nullptr;      ///< Shown on hover; nullptr: none.
};

/// Which footer button was pressed this frame.
enum class DialogFooterAction : std::uint8_t
{
    None,
    Primary,
    Secondary,
    Leading,
};

/// A dialog action button's preferred width: wide enough for the widest of @p labels, and never
/// below @p minWidthEm (see UI::DialogMetrics::computeActionButtonWidth()). Both action buttons of a
/// footer share it, so the pair reads as a pair.
[[nodiscard]] inline float footerButtonWidth(std::initializer_list<const char*> labels, float minWidthEm)
{
    float widest = 0.0F;
    for (const char* label : labels)
    {
        widest = std::max(widest, ImGui::CalcTextSize(label).x);
    }
    return UI::DialogMetrics::computeActionButtonWidth(widest, ImGui::GetFontSize(), minWidthEm);
}

/// Width of a footer's leading button, which is sized to its own label.
[[nodiscard]] inline float footerLeadingButtonWidth(const char* label)
{
    return ImGui::CalcTextSize(label).x + (ImGui::GetStyle().FramePadding.x * 2.0F);
}

namespace Detail
{
inline bool drawFooterButton(const DialogFooterButton& button, float width)
{
    const auto& scheme = UI::Theme::get().scheme();
    bool clicked = false;
    if (button.fills != nullptr)
    {
        clicked = filledButton(button.label, ImVec2(width, 0.0F), *button.fills, scheme.textPrimary, scheme.windowBg);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textPrimary);
        clicked = ImGui::Button(button.label, ImVec2(width, 0.0F));
        ImGui::PopStyleColor();
    }
    if (button.tooltip != nullptr)
    {
        ImGui::SetItemTooltip("%s", button.tooltip);
    }
    return clicked;
}
} // namespace Detail

/// The footer every dialog ends with (#1200): a separator, then [leading] ... [secondary][primary],
/// with the primary action rightmost. See ChromeLayout::placeDialogFooter() for how the row fits a
/// narrow dialog, and ChromeLayout::dialogFooterHeight() for the height to reserve for it.
///
/// @param primary               The dialog's main action (OK, Save, Kill): always the rightmost button.
/// @param secondary             Usually Cancel; a null label for none.
/// @param preferredButtonWidth  Width of each action button when the row has room, e.g. from
///                              footerButtonWidth().
/// @param leading               A left-aligned extra button at its own width (Reset to defaults);
///                              a null label for none.
/// @param maxRowWidth           Most the row may grow an auto-fitting dialog to; 0: the dialog's
///                              current content width.
/// @return The button pressed this frame, if any.
inline DialogFooterAction dialogFooter(const DialogFooterButton& primary,
                                       const DialogFooterButton& secondary,
                                       float preferredButtonWidth,
                                       const DialogFooterButton& leading = {},
                                       float maxRowWidth = 0.0F)
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const ImGuiStyle& style = ImGui::GetStyle();
    const bool hasLeading = leading.label != nullptr;
    const bool hasSecondary = secondary.label != nullptr;
    const auto placement = ChromeLayout::placeDialogFooter({
        .availWidth = ImGui::GetContentRegionAvail().x,
        .maxRowWidth = maxRowWidth,
        .spacing = style.ItemSpacing.x,
        .preferredButtonWidth = preferredButtonWidth,
        .actionCount = hasSecondary ? 2U : 1U,
        .leadingWidth = hasLeading ? footerLeadingButtonWidth(leading.label) : 0.0F,
    });

    DialogFooterAction action = DialogFooterAction::None;
    const float rowStartX = ImGui::GetCursorPosX();
    if (hasLeading)
    {
        if (Detail::drawFooterButton(leading, footerLeadingButtonWidth(leading.label)))
        {
            action = DialogFooterAction::Leading;
        }
        if (!placement.leadingOnOwnRow)
        {
            ImGui::SameLine();
        }
    }
    if (hasSecondary)
    {
        ImGui::SetCursorPosX(rowStartX + placement.secondaryX);
        if (Detail::drawFooterButton(secondary, placement.buttonWidth))
        {
            action = DialogFooterAction::Secondary;
        }
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(rowStartX + placement.primaryX);
    if (Detail::drawFooterButton(primary, placement.buttonWidth))
    {
        action = DialogFooterAction::Primary;
    }
    return action;
}

} // namespace UI::Widgets
