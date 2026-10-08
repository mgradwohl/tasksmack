#include "ProcessConnectionsView.h"

#include "Platform/IProcessConnections.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace App
{

namespace
{

/// Draws @p text in the current cell, clipped by the column, with the whole of it in a tooltip when
/// it does not fit (a long IPv6 address in a narrow pane).
void cellText(std::string_view text)
{
    // The width the text really has, taken before the text moves the cursor.
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ImGui::CalcTextSize(text.data(), text.data() + text.size()).x > available)
    {
        if (ImGui::BeginTooltip())
        {
            ImGui::TextUnformatted(text.data(), text.data() + text.size());
            ImGui::EndTooltip();
        }
    }
}

} // namespace

void ProcessConnectionsView::render(bool hasConnections)
{
    if (!hasConnections)
    {
        return; // Windows (for now) and synthetic runs: no section at all
    }
    // Collapsed by default: nothing is read until it is opened.
    if (!ImGui::CollapsingHeader(ICON_FA_NETWORK_WIRED "  Connections###ProcessConnections"))
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
    if (m_Status != Platform::ConnectionsReadStatus::Ok)
    {
        // Not an empty table: an empty one would read as "this process has no connections".
        const std::string text = Detail::connectionsStatusLine(m_Status, m_Detail);
        const ImVec4 color = (m_Status == Platform::ConnectionsReadStatus::PermissionDenied) ? scheme.textWarning : scheme.textMuted;
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (m_Rows.empty())
    {
        ImGui::TextColored(scheme.textMuted, "No TCP or UDP sockets");
        return;
    }
    const std::string count = Detail::connectionsCountText(m_Rows.size());
    ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
    ImGui::TextUnformatted(count.c_str());
    ImGui::PopStyleColor();
    renderTable();
}

void ProcessConnectionsView::renderTable()
{
    // Tall enough for the rows up to a cap, plus the header; beyond it the table scrolls on its own.
    const std::size_t shownRows = std::min(m_Rows.size(), Detail::CONNECTIONS_TABLE_MAX_VISIBLE_ROWS);
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    const float tableHeight = rowHeight * static_cast<float>(shownRows + 1); // + 1: the header row

    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##ConnectionsTable", 4, TABLE_FLAGS, ImVec2(0.0F, tableHeight)))
    {
        return;
    }
    using Detail::ConnectionsColumn;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("PROTO", ImGuiTableColumnFlags_WidthStretch, 0.10F, static_cast<ImGuiID>(ConnectionsColumn::Protocol));
    ImGui::TableSetupColumn("LOCAL ADDRESS", ImGuiTableColumnFlags_WidthStretch, 0.34F, static_cast<ImGuiID>(ConnectionsColumn::Local));
    ImGui::TableSetupColumn("REMOTE ADDRESS", ImGuiTableColumnFlags_WidthStretch, 0.34F, static_cast<ImGuiID>(ConnectionsColumn::Remote));
    ImGui::TableSetupColumn("STATE",
                            ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort,
                            0.22F,
                            static_cast<ImGuiID>(ConnectionsColumn::State));
    ImGui::TableHeadersRow();

    // A header click (or the first frame's default sort): re-sort the rows once, not every frame.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
    {
        if (specs->SpecsCount > 0)
        {
            const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
            setSort(static_cast<ConnectionsColumn>(spec.ColumnUserID), spec.SortDirection != ImGuiSortDirection_Descending);
        }
        specs->SpecsDirty = false;
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_Rows.size()), rowHeight);
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const Row& row = m_Rows[static_cast<std::size_t>(i)];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableNextColumn();
            cellText(row.protocol);
            ImGui::TableNextColumn();
            cellText(row.local);
            ImGui::TableNextColumn();
            cellText(row.remote);
            ImGui::TableNextColumn();
            cellText(row.state);
        }
    }
    ImGui::EndTable();
}

} // namespace App
