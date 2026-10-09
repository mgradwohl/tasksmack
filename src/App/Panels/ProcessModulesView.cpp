#include "ProcessModulesView.h"

#include "Platform/IProcessModules.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>

#include <misc/cpp/imgui_stdlib.h>

namespace App
{

namespace
{

/// Draws @p text in the current cell, clipped by the column, with the whole of it in a tooltip when
/// it does not fit (a long path in a narrow pane).
void cellText(std::string_view text)
{
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ImGui::CalcTextSize(text.data(), text.data() + text.size()).x > available)
    {
        ImGui::SetTooltip("%.*s", static_cast<int>(text.size()), text.data());
    }
}

} // namespace

void ProcessModulesView::render(bool hasModules)
{
    if (!hasModules)
    {
        return; // synthetic runs: no section at all
    }
    // The count, once a read has listed the modules, formatted only when it changes; the ### keeps
    // the header's ID (and its open state) the same as the count changes.
    const bool listed = m_HasRead && m_Status == Platform::ModulesReadStatus::Ok;
    if (listed && m_LabelCount != m_Rows.size())
    {
        m_LabelCount = m_Rows.size();
        m_CountLabel = std::format(ICON_FA_PLUG "  Modules ({})###ProcessModules", m_LabelCount);
    }
    // Collapsed by default: nothing is read until it is opened.
    if (!ImGui::CollapsingHeader(listed ? m_CountLabel.c_str() : ICON_FA_PLUG "  Modules###ProcessModules"))
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
    if (m_Status != Platform::ModulesReadStatus::Ok)
    {
        // A muted note, not an empty table (which would read as "no modules") and not an error:
        // protected and elevated processes refuse this routinely.
        std::string text(Detail::modulesStatusText(m_Status));
        if (m_Status == Platform::ModulesReadStatus::Failed && !m_Detail.empty())
        {
            text += ": " + m_Detail;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (m_Rows.empty())
    {
        ImGui::TextColored(scheme.textMuted, "No modules loaded");
        return;
    }
    renderTable();
}

void ProcessModulesView::renderTable()
{
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##ModulesFilter", ICON_FA_FILTER "  Filter by name or path", &m_Filter);
    const std::span<const std::size_t> visible = filteredRows();
    if (visible.empty())
    {
        ImGui::TextColored(UI::Theme::get().scheme().textMuted, "No modules match the filter");
        return;
    }

    // Tall enough for the rows up to a cap, plus the header; beyond it the table scrolls on its own.
    const std::size_t shownRows = std::min(visible.size(), Detail::MODULES_TABLE_MAX_VISIBLE_ROWS);
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    const float tableHeight = rowHeight * static_cast<float>(shownRows + 1); // + 1: the header row

    // No module carries a version on Linux: that table has no Version column (its own ID, so the two
    // layouts keep their own column widths).
    const bool versions = hasVersions();
    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(
            versions ? "##ModulesTable" : "##ModulesTableNoVersion", versions ? 5 : 4, TABLE_FLAGS, ImVec2(0.0F, tableHeight)))
    {
        return;
    }
    using Detail::ModulesColumn;
    const auto setupColumn = [](const char* name, float weight, ModulesColumn column, ImGuiTableColumnFlags flags = 0)
    {
        ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthStretch | flags, weight, static_cast<ImGuiID>(column));
    };
    ImGui::TableSetupScrollFreeze(0, 1);
    setupColumn("NAME", 0.20F, ModulesColumn::Name, ImGuiTableColumnFlags_DefaultSort);
    if (versions)
    {
        setupColumn("VERSION", 0.14F, ModulesColumn::Version);
    }
    setupColumn("BASE", 0.14F, ModulesColumn::Base);
    setupColumn("SIZE", 0.09F, ModulesColumn::Size);
    setupColumn("PATH", 0.43F, ModulesColumn::Path);
    ImGui::TableHeadersRow();

    // A header click (or the first frame's default sort): re-sort the rows once, not every frame.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
    {
        if (specs->SpecsCount > 0)
        {
            const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
            setSort(static_cast<ModulesColumn>(spec.ColumnUserID), spec.SortDirection != ImGuiSortDirection_Descending);
        }
        specs->SpecsDirty = false;
    }

    // setSort() may have re-sorted: take the rows' current order.
    const std::span<const std::size_t> ordered = filteredRows();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(ordered.size()), rowHeight);
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const Row& row = m_Rows[ordered[static_cast<std::size_t>(i)]];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableNextColumn();
            cellText(row.name);
            if (versions)
            {
                ImGui::TableNextColumn();
                cellText(row.version);
            }
            ImGui::TableNextColumn();
            cellText(row.base);
            ImGui::TableNextColumn();
            cellText(row.size);
            ImGui::TableNextColumn();
            cellText(row.module.path);
        }
    }
    ImGui::EndTable();
}

} // namespace App
