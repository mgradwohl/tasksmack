#include "ProcessConnectionsView.h"

#include "Domain/ProcessConnections.h"
#include "Platform/IProcessConnections.h"
#include "UI/CellText.h"
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

/// Draws an endpoint in the current cell. One too long for its column ("[2605:59ca:...:4a31]:50747" in
/// a narrow pane) is cut in the middle of its address, never its port: the port is what tells a
/// process's sockets apart, and with it cut off, rows sharing an address read the same (#1942). The
/// whole endpoint is the cell's tooltip.
void endpointCell(std::string_view endpoint)
{
    UI::Widgets::cellTextKeepingTail(endpoint, Domain::Connections::endpointPortSuffixLength(endpoint));
}

} // namespace

void ProcessConnectionsView::render(bool hasConnections)
{
    if (!hasConnections)
    {
        return; // Synthetic runs, or a reader without connections: no section at all
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
        // Another user's process: an administrator can read it (#2309). The lock and the words.
        if (offersAuthenticateToRead())
        {
            if (ImGui::SmallButton(ICON_FA_LOCK "  Authenticate to read"))
            {
                requestAuthorizedRead();
            }
            ImGui::SetItemTooltip("Asks for an administrator's password, then reads it as the administrator");
        }
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
            UI::Widgets::cellText(row.protocol);
            ImGui::TableNextColumn();
            endpointCell(row.local);
            ImGui::TableNextColumn();
            endpointCell(row.remote);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.state);
        }
    }
    ImGui::EndTable();
}

} // namespace App
