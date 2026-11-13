#include "ProcessMemoryMapView.h"

#include "AuthenticateToReadButton.h"
#include "Domain/ProcessMemoryMap.h"
#include "Platform/IProcessMemoryMap.h"
#include "UI/CellText.h"
#include "UI/FrameOutline.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <string>

#include <misc/cpp/imgui_stdlib.h>

namespace App
{

void ProcessMemoryMapView::render(bool hasMemoryMap)
{
    if (!hasMemoryMap)
    {
        return; // synthetic runs: no section at all
    }
    // The count, once a read has listed the regions, formatted only when it changes; the ### keeps the
    // header's ID (and its open state) the same as the count changes.
    const bool listed = m_HasRead && m_Status == Platform::MemoryMapReadStatus::Ok;
    if (listed && m_LabelCount != m_Rows.size())
    {
        m_LabelCount = m_Rows.size();
        m_CountLabel = std::format(ICON_FA_MEMORY "  Memory map ({})###ProcessMemoryMap", m_LabelCount);
    }
    // Collapsed by default: nothing is read until it is opened.
    if (!ImGui::CollapsingHeader(listed ? m_CountLabel.c_str() : ICON_FA_MEMORY "  Memory map###ProcessMemoryMap"))
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
    if (m_Status != Platform::MemoryMapReadStatus::Ok)
    {
        // A muted note, not an empty table (which would read as "no memory") and not an error: protected and
        // elevated processes refuse this routinely.
        std::string text(Detail::memoryMapStatusText(m_Status));
        if (m_Status == Platform::MemoryMapReadStatus::Failed && !m_Detail.empty())
        {
            text += ": " + m_Detail;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
        Detail::drawAuthenticateToReadButton(*this);
        return;
    }
    if (m_Truncated)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
        ImGui::TextWrapped("Showing the first %zu regions", m_Rows.size());
        ImGui::PopStyleColor();
    }
    if (m_Rows.empty())
    {
        ImGui::TextColored(scheme.textMuted, "No memory mapped");
        return;
    }
    renderTotals();
    renderTable();
}

void ProcessMemoryMapView::renderTotals()
{
    // The "Total" row's totals say which sizes the platform measured: a column for each, so Windows shows no
    // empty Resident or PSS column and Linux no Reserved one.
    const Domain::MemoryMap::Totals& all = m_Totals.back().totals;
    const bool reserved = all.reservedBytes > 0;
    const bool resident = all.residentBytes.has_value();
    const bool pss = all.pssBytes.has_value();
    const bool split = all.privateBytes.has_value() || all.sharedBytes.has_value();
    const bool swap = all.swapBytes.has_value();
    const int columns = 3 + (reserved ? 1 : 0) + (resident ? 1 : 0) + (pss ? 1 : 0) + (split ? 2 : 0) + (swap ? 1 : 0);

    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    // Windows' layout (reserved space, no smaps sizes) has its own ID, so each keeps its own widths.
    if (!ImGui::BeginTable(reserved ? "##MemoryMapTotalsReserved" : "##MemoryMapTotals", columns, TABLE_FLAGS))
    {
        return;
    }
    ImGui::TableSetupColumn("KIND", ImGuiTableColumnFlags_WidthStretch, 0.22F);
    ImGui::TableSetupColumn("REGIONS", ImGuiTableColumnFlags_WidthStretch, 0.10F);
    ImGui::TableSetupColumn("SIZE", ImGuiTableColumnFlags_WidthStretch, 0.12F);
    if (reserved)
    {
        ImGui::TableSetupColumn("RESERVED", ImGuiTableColumnFlags_WidthStretch, 0.12F);
    }
    if (resident)
    {
        ImGui::TableSetupColumn("RESIDENT", ImGuiTableColumnFlags_WidthStretch, 0.12F);
    }
    if (pss)
    {
        ImGui::TableSetupColumn("PSS", ImGuiTableColumnFlags_WidthStretch, 0.12F);
    }
    if (split)
    {
        ImGui::TableSetupColumn("PRIVATE", ImGuiTableColumnFlags_WidthStretch, 0.12F);
        ImGui::TableSetupColumn("SHARED", ImGuiTableColumnFlags_WidthStretch, 0.12F);
    }
    if (swap)
    {
        ImGui::TableSetupColumn("SWAP", ImGuiTableColumnFlags_WidthStretch, 0.12F);
    }
    ImGui::TableHeadersRow();
    for (const TotalsRow& row : m_Totals)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        UI::Widgets::cellText(row.label);
        ImGui::TableNextColumn();
        UI::Widgets::cellText(row.regions);
        ImGui::TableNextColumn();
        UI::Widgets::cellText(row.size);
        const auto cell = [](bool shown, const std::string& text)
        {
            if (shown)
            {
                ImGui::TableNextColumn();
                UI::Widgets::cellText(text);
            }
        };
        cell(reserved, row.reserved);
        cell(resident, row.resident);
        cell(pss, row.pss);
        cell(split, row.privateBytes);
        cell(split, row.shared);
        cell(swap, row.swap);
    }
    ImGui::EndTable();
    ImGui::Spacing();
}

void ProcessMemoryMapView::renderTable()
{
    ImGui::SetNextItemWidth(-1.0F);
    {
        const UI::Widgets::FrameOutlineScope frameOutline;
        ImGui::InputTextWithHint("##MemoryMapFilter", ICON_FA_FILTER "  Filter by kind, protection or path", &m_Filter);
    }
    const std::span<const std::size_t> visible = filteredRows();
    if (visible.empty())
    {
        ImGui::TextColored(UI::Theme::get().scheme().textMuted, "No regions match the filter");
        return;
    }

    // Tall enough for the rows up to a cap, plus the header; beyond it the table scrolls on its own.
    const std::size_t shownRows = std::min(visible.size(), Detail::MEMORY_MAP_TABLE_MAX_VISIBLE_ROWS);
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    const float tableHeight = rowHeight * static_cast<float>(shownRows + 1); // + 1: the header row

    // No region carries a resident size on Windows: that table has no RESIDENT column (its own ID, so the
    // two layouts keep their own column widths).
    const bool resident = hasResident();
    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(
            resident ? "##MemoryMapTable" : "##MemoryMapTableNoResident", resident ? 6 : 5, TABLE_FLAGS, ImVec2(0.0F, tableHeight)))
    {
        return;
    }
    using Detail::MemoryMapColumn;
    const auto setupColumn = [](const char* name, float weight, MemoryMapColumn column, ImGuiTableColumnFlags flags = 0)
    {
        ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthStretch | flags, weight, static_cast<ImGuiID>(column));
    };
    ImGui::TableSetupScrollFreeze(0, 1);
    setupColumn("ADDRESS", 0.15F, MemoryMapColumn::Address, ImGuiTableColumnFlags_DefaultSort);
    setupColumn("SIZE", 0.09F, MemoryMapColumn::Size);
    setupColumn("KIND", 0.11F, MemoryMapColumn::Kind);
    setupColumn("PROTECTION", 0.15F, MemoryMapColumn::Protection);
    if (resident)
    {
        setupColumn("RESIDENT", 0.09F, MemoryMapColumn::Resident);
    }
    setupColumn("PATH", 0.41F, MemoryMapColumn::Path);
    ImGui::TableHeadersRow();

    // A header click (or the first frame's default sort): re-sort the rows once, not every frame.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
    {
        if (specs->SpecsCount > 0)
        {
            const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
            setSort(static_cast<MemoryMapColumn>(spec.ColumnUserID), spec.SortDirection != ImGuiSortDirection_Descending);
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
            UI::Widgets::cellText(row.address);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.size);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.kind);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.protection);
            if (resident)
            {
                ImGui::TableNextColumn();
                UI::Widgets::cellText(row.resident);
            }
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.path);
        }
    }
    ImGui::EndTable();
}

} // namespace App
