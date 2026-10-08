#include "ProcessEnvironmentView.h"

#include "Platform/IProcessEnvironment.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

#include <misc/cpp/imgui_stdlib.h>

namespace App
{

namespace
{

/// Longest text drawn in a cell, in bytes; a longer value is cut there and shown whole (up to
/// MAX_TOOLTIP_BYTES) in its tooltip. Measuring a multi-megabyte value every frame would be wasted:
/// the column shows a few dozen characters of it.
constexpr std::size_t MAX_CELL_BYTES = 512;
/// Longest text a tooltip shows, in bytes.
constexpr std::size_t MAX_TOOLTIP_BYTES = 4096;
/// A tooltip wraps at this many ems.
constexpr float TOOLTIP_WRAP_EMS = 40.0F;

/// @p text cut to at most @p maxBytes without splitting a UTF-8 sequence (the parser left only
/// well-formed ones, so stepping back over continuation bytes finds a boundary).
[[nodiscard]] std::string_view truncateUtf8(std::string_view text, std::size_t maxBytes)
{
    if (text.size() <= maxBytes)
    {
        return text;
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U)
    {
        --cut;
    }
    return text.substr(0, cut);
}

/// Draws @p text in the current cell, clipped by the column, with the whole of it in a tooltip when
/// it does not fit.
void cellText(std::string_view text)
{
    const std::string_view shown = truncateUtf8(text, MAX_CELL_BYTES);
    // The width the text really has: what is left of the cell after its padding and anything drawn
    // before it on the line (a reveal button), taken before the text moves the cursor.
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(shown.data(), shown.data() + shown.size());
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
    {
        return;
    }
    const float textWidth = ImGui::CalcTextSize(shown.data(), shown.data() + shown.size()).x;
    if (Detail::environmentCellNeedsTooltip(shown.size(), text.size(), textWidth, available))
    {
        const std::string_view full = truncateUtf8(text, MAX_TOOLTIP_BYTES);
        if (ImGui::BeginTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * TOOLTIP_WRAP_EMS);
            ImGui::TextUnformatted(full.data(), full.data() + full.size());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

} // namespace

void ProcessEnvironmentView::render(bool hasEnvironment)
{
    if (!hasEnvironment)
    {
        return; // Windows and synthetic runs: no section at all
    }
    // Collapsed by default: nothing is read until it is opened.
    if (!ImGui::CollapsingHeader(ICON_FA_LIST "  Environment###ProcessEnvironment"))
    {
        return;
    }
    markDrawnOpen();

    const auto& scheme = UI::Theme::get().scheme();
    if (!m_HasRead)
    {
        ImGui::TextColored(scheme.textMuted, "Reading...");
        return;
    }
    if (m_Status != Platform::EnvironmentReadStatus::Ok)
    {
        // Not an empty table: an empty one would read as "this process has no environment".
        const std::string_view text = Detail::environmentStatusText(m_Status);
        const ImVec4 color = (m_Status == Platform::EnvironmentReadStatus::PermissionDenied) ? scheme.textWarning : scheme.textMuted;
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopStyleColor();
        return;
    }
    if (m_Rows.empty())
    {
        ImGui::TextColored(scheme.textMuted, "No environment variables");
        return;
    }
    renderTable();
}

void ProcessEnvironmentView::renderTable()
{
    if (m_Rows.size() > Detail::ENVIRONMENT_FILTER_MIN_ROWS)
    {
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##EnvironmentFilter", ICON_FA_FILTER "  Filter by name or value", &m_Filter);
    }
    const std::span<const std::size_t> visible = filteredRows();
    if (visible.empty())
    {
        ImGui::TextColored(UI::Theme::get().scheme().textMuted, "No variables match the filter");
        return;
    }

    // Tall enough for the rows up to a cap, plus the header; beyond it the table scrolls on its own.
    const std::size_t shownRows = std::min(visible.size(), Detail::ENVIRONMENT_TABLE_MAX_VISIBLE_ROWS);
    const float rowHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    const float tableHeight = rowHeight * static_cast<float>(shownRows + 1); // + 1: the header row

    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                            ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##EnvironmentTable", 2, TABLE_FLAGS, ImVec2(0.0F, tableHeight)))
    {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch, 0.35F);
    ImGui::TableSetupColumn("VALUE", ImGuiTableColumnFlags_WidthStretch, 0.65F);
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()), rowHeight);
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const Row& row = m_Rows[visible[static_cast<std::size_t>(i)]];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            cellText(row.name);

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (row.secret)
            {
                // Per-row reveal: shows this one value until the selection changes (or it is hidden again).
                const bool revealed = isRevealed(row);
                ImGui::PushID(i);
                if (ImGui::SmallButton(revealed ? ICON_FA_EYE_SLASH "##Reveal" : ICON_FA_EYE "##Reveal"))
                {
                    toggleReveal(row);
                }
                ImGui::SetItemTooltip("%s", revealed ? "Hide value" : "Show value");
                ImGui::PopID();
                ImGui::SameLine();
            }
            // Masked: the bullets only. The value itself is never handed to ImGui, not even for a tooltip.
            cellText(isMasked(row) ? Detail::MASKED_ENVIRONMENT_VALUE : std::string_view{row.value});
        }
    }
    ImGui::EndTable();
}

} // namespace App
