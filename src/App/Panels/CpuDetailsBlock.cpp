#include "CpuDetailsBlock.h"

#include "App/Panels/CpuDetailsText.h"
#include "UI/ChromeWidgets.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/LineLayout.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace App::CpuDetailsBlock
{

namespace
{

/// Space after each value column, in ems, so one pair's value stays clear of the next pair's label.
constexpr float PAIR_GAP_EM = 1.5F;
/// A font-size change smaller than this is not a change (the size moves in whole preset steps).
constexpr float FONT_SIZE_TOLERANCE_PX = 0.01F;

void textView(std::string_view text)
{
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

[[nodiscard]] float textWidth(std::string_view text)
{
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

/// Measure each row's columns, only when the rows or the font changed since the last measurement.
void measure(std::span<const CpuDetailsText::Row> rows, std::uint64_t rowsGeneration, MeasuredRows& measured)
{
    const float em = ImGui::GetFontSize();
    const bool current = measured.rowsGeneration == rowsGeneration && std::abs(measured.fontSize - em) <= FONT_SIZE_TOLERANCE_PX &&
                         measured.labelWidths.size() == rows.size();
    if (current)
    {
        return;
    }
    measured.labelWidths.resize(rows.size());
    measured.valueWidths.resize(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        // Measured at the current font, with the em-scaled gap every label column has (#1200)
        measured.labelWidths[i] = UI::LineLayout::labelColumnWidth(textWidth(rows[i].label), em);
        measured.valueWidths[i] = textWidth(rows[i].value) + (PAIR_GAP_EM * em);
    }
    measured.rowsGeneration = rowsGeneration;
    measured.fontSize = em;
}

} // namespace

bool render(const Content& content, MeasuredRows& measured)
{
    const auto& theme = UI::Theme::get();

    // The heading line is the toggle: an invisible button one text line tall across the width, then
    // the caret and heading drawn over it. Text takes no input, so clicks reach the button, and the
    // cursor ends where the line of text would, so the collapsed block is exactly one text line.
    const ImVec2 lineStart = ImGui::GetCursorScreenPos();
    const float lineWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0F);
    const bool toggled = ImGui::InvisibleButton("##CpuDetailsToggle", ImVec2(lineWidth, ImGui::GetTextLineHeight()));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
    {
        ImGui::SetTooltip("%s", content.expanded ? "Collapse CPU Details" : "Expand CPU Details");
    }
    ImGui::SetCursorScreenPos(lineStart);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textMuted);
    textView(content.expanded ? std::string_view{ICON_FA_CARET_DOWN} : std::string_view{ICON_FA_CARET_RIGHT});
    ImGui::PopStyleColor();
    ImGui::SameLine();
    (void) UI::Widgets::sectionHeader(ICON_FA_MICROCHIP, "CPU Details", content.expanded ? content.cpuModel : content.collapsedSummary);
    // The heading line leaves the cursor where the button did (moving it there explicitly would
    // extend the window past its items, which ImGui rejects).

    const std::span<const CpuDetailsText::Row> rows = content.rows;
    if (!content.expanded || rows.empty())
    {
        return toggled;
    }

    measure(rows, content.rowsGeneration, measured);
    const float perPairExtra = ImGui::GetStyle().CellPadding.x * 4.0F; // Two cells' padding a pair
    const CpuDetailsText::ColumnLayout layout =
        CpuDetailsText::columnLayout(measured.labelWidths, measured.valueWidths, ImGui::GetContentRegionAvail().x, perPairExtra);

    // Sized to its columns (NoHostExtendX), which columnLayout() has widened to the full width.
    constexpr ImGuiTableFlags TABLE_FLAGS =
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_NoHostExtendX;
    if (!ImGui::BeginTable("##CpuDetails", static_cast<int>(layout.pairs * 2), TABLE_FLAGS))
    {
        return toggled;
    }
    for (std::size_t pair = 0; pair < layout.pairs; ++pair)
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, layout.labelWidths[pair]);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, layout.valueWidths[pair]);
    }
    for (std::size_t line = 0; line < layout.rowsPerPair; ++line)
    {
        ImGui::TableNextRow();
        for (std::size_t pair = 0; pair < layout.pairs; ++pair)
        {
            // Filled top to bottom, then left to right, so related rows stay together in a column.
            const std::size_t index = (pair * layout.rowsPerPair) + line;
            ImGui::TableNextColumn();
            if (index >= rows.size())
            {
                ImGui::TableNextColumn();
                continue;
            }
            const CpuDetailsText::Row& row = rows[index];
            ImGui::PushStyleColor(ImGuiCol_Text, theme.scheme().textPrimary); // As Process Details' Identity/Runtime
            textView(row.label);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, row.available ? theme.scheme().textPrimary : theme.scheme().textMuted);
            textView(row.value);
            ImGui::PopStyleColor();
            if (!row.tooltip.empty() && ImGui::BeginItemTooltip())
            {
                textView(row.tooltip);
                ImGui::EndTooltip();
            }
        }
    }
    ImGui::EndTable();
    return toggled;
}

} // namespace App::CpuDetailsBlock
