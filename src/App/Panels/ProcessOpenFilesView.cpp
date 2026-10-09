#include "ProcessOpenFilesView.h"

#include "Platform/IProcessOpenFiles.h"
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
/// it does not fit.
void cellText(std::string_view text)
{
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(text.data(), text.data() + text.size());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ImGui::CalcTextSize(text.data(), text.data() + text.size()).x > available)
    {
        ImGui::SetTooltip("%.*s", static_cast<int>(text.size()), text.data());
    }
}

void mutedWrapped(const std::string& text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::get().scheme().textMuted);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

} // namespace

void ProcessOpenFilesView::render(bool hasOpenFiles)
{
    if (!hasOpenFiles)
    {
        return; // synthetic runs: no section at all
    }
    const bool listed = m_HasRead && m_Status == Platform::OpenFilesReadStatus::Ok;
    if (listed && m_LabelCount != m_Rows.size())
    {
        m_LabelCount = m_Rows.size();
        m_CountLabel = std::format(ICON_FA_FILE "  Open files ({})###ProcessOpenFiles", m_LabelCount);
    }
    // Collapsed by default: nothing is read until it is opened.
    if (!ImGui::CollapsingHeader(listed ? m_CountLabel.c_str() : ICON_FA_FILE "  Open files###ProcessOpenFiles"))
    {
        return;
    }
    markDrawnOpen();

    if (!m_HasRead)
    {
        ImGui::TextColored(UI::Theme::get().scheme().textMuted, "Reading...");
        return;
    }
    if (m_Status != Platform::OpenFilesReadStatus::Ok)
    {
        // A muted note, not an empty table (which would read as "no files open") and not an error.
        std::string text(Detail::openFilesStatusText(m_Status));
        if (m_Status == Platform::OpenFilesReadStatus::Failed && !m_Detail.empty())
        {
            text += ": " + m_Detail;
        }
        mutedWrapped(text);
        return;
    }
    if (m_Truncated)
    {
        mutedWrapped(std::format("Showing the first {} open files", m_Rows.size()));
    }
    if (m_NamesIncomplete)
    {
        mutedWrapped("Some handles were not named: one did not answer in time, or names are paused after earlier ones did not");
    }
    if (m_Rows.empty())
    {
        ImGui::TextColored(UI::Theme::get().scheme().textMuted, "No open files");
        return;
    }
    renderTable();
}

void ProcessOpenFilesView::renderTable()
{
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##OpenFilesFilter", ICON_FA_FILTER "  Filter by path or type", &m_Filter);
    const std::span<const std::size_t> visible = filteredRows();
    if (visible.empty())
    {
        ImGui::TextColored(UI::Theme::get().scheme().textMuted, "No open files match the filter");
        return;
    }

    const std::size_t shownRows = std::min(visible.size(), Detail::OPEN_FILES_TABLE_MAX_VISIBLE_ROWS);
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    const float tableHeight = rowHeight * static_cast<float>(shownRows + 1); // + 1: the header row

    // No open flags on Windows: that table has no Mode column (its own ID, so each layout keeps its widths).
    const bool modes = hasModes();
    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(modes ? "##OpenFilesTable" : "##OpenFilesTableNoMode", modes ? 4 : 3, TABLE_FLAGS, ImVec2(0.0F, tableHeight)))
    {
        return;
    }
    using Detail::OpenFilesColumn;
    const auto setupColumn = [](const char* name, float weight, OpenFilesColumn column, ImGuiTableColumnFlags flags = 0)
    {
        ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthStretch | flags, weight, static_cast<ImGuiID>(column));
    };
    ImGui::TableSetupScrollFreeze(0, 1);
    setupColumn(m_HexDescriptors ? "HANDLE" : "FD", 0.10F, OpenFilesColumn::Descriptor, ImGuiTableColumnFlags_DefaultSort);
    setupColumn("TYPE", 0.12F, OpenFilesColumn::Type);
    if (modes)
    {
        setupColumn("MODE", 0.10F, OpenFilesColumn::Mode);
    }
    setupColumn("PATH", modes ? 0.68F : 0.78F, OpenFilesColumn::Path);
    ImGui::TableHeadersRow();

    // A header click (or the first frame's default sort): re-sort the rows once, not every frame.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
    {
        if (specs->SpecsCount > 0)
        {
            const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
            setSort(static_cast<OpenFilesColumn>(spec.ColumnUserID), spec.SortDirection != ImGuiSortDirection_Descending);
        }
        specs->SpecsDirty = false;
    }

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
            cellText(row.descriptor);
            ImGui::TableNextColumn();
            cellText(row.type);
            if (modes)
            {
                ImGui::TableNextColumn();
                cellText(row.mode);
            }
            ImGui::TableNextColumn();
            cellText(row.path);
        }
    }
    ImGui::EndTable();
}

} // namespace App
