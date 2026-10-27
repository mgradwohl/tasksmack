#include "ProcessThreadsView.h"

#include "Platform/IProcessThreads.h"
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

void ProcessThreadsView::render(bool hasThreads)
{
    if (!hasThreads)
    {
        return; // synthetic runs, and Windows for now: no section at all
    }
    // The count, formatted only when it changes; the ### keeps the header's ID (and its open state).
    const bool listed = m_HasRead && m_Status == Platform::ThreadsReadStatus::Ok;
    if (listed && m_LabelCount != m_Rows.size())
    {
        m_LabelCount = m_Rows.size();
        m_CountLabel = std::format(ICON_FA_LIST "  Threads ({})###ProcessThreads", m_LabelCount);
    }
    // Collapsed by default: nothing is read until it is opened.
    if (!ImGui::CollapsingHeader(listed ? m_CountLabel.c_str() : ICON_FA_LIST "  Threads###ProcessThreads"))
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
    if (m_Status != Platform::ThreadsReadStatus::Ok)
    {
        // A muted note, not an empty table, which would read as "no threads".
        std::string text(Detail::threadsStatusText(m_Status));
        if (m_Status == Platform::ThreadsReadStatus::Failed && !m_Detail.empty())
        {
            text += ": " + m_Detail;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (m_Truncated)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, scheme.textMuted);
        ImGui::TextWrapped("Showing the first %zu threads", m_Rows.size());
        ImGui::PopStyleColor();
    }
    renderTable();
}

void ProcessThreadsView::renderTable()
{
    ImGui::SetNextItemWidth(-1.0F);
    {
        const UI::Widgets::FrameOutlineScope frameOutline;
        ImGui::InputTextWithHint("##ThreadsFilter", ICON_FA_FILTER "  Filter by name or TID", &m_Filter);
    }
    const std::span<const std::size_t> visible = filteredRows();
    if (visible.empty())
    {
        ImGui::TextColored(UI::Theme::get().scheme().textMuted, "No threads match the filter");
        return;
    }

    // Tall enough for the rows up to a cap, plus the header; beyond it the table scrolls on its own.
    const std::size_t shownRows = std::min(visible.size(), Detail::THREADS_TABLE_MAX_VISIBLE_ROWS);
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    const float tableHeight = rowHeight * static_cast<float>(shownRows + 1); // + 1: the header row

    constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##ThreadsTable", 6, TABLE_FLAGS, ImVec2(0.0F, tableHeight)))
    {
        return;
    }
    using Detail::ThreadsColumn;
    const auto setupColumn = [](const char* name, float weight, ThreadsColumn column, ImGuiTableColumnFlags flags = 0)
    {
        ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthStretch | flags, weight, static_cast<ImGuiID>(column));
    };
    ImGui::TableSetupScrollFreeze(0, 1);
    setupColumn("TID", 0.12F, ThreadsColumn::Tid);
    setupColumn("NAME", 0.30F, ThreadsColumn::Name);
    setupColumn("STATE", 0.16F, ThreadsColumn::State);
    setupColumn("CPU %", 0.12F, ThreadsColumn::Cpu, ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
    setupColumn("CPU TIME", 0.18F, ThreadsColumn::CpuTime, ImGuiTableColumnFlags_PreferSortDescending);
    setupColumn("NICE", 0.12F, ThreadsColumn::Nice);
    ImGui::TableHeadersRow();

    // A header click (or the first frame's default sort): re-sort the rows once, not every frame.
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty)
    {
        if (specs->SpecsCount > 0)
        {
            const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
            setSort(static_cast<ThreadsColumn>(spec.ColumnUserID), spec.SortDirection != ImGuiSortDirection_Descending);
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
            UI::Widgets::cellText(row.tid);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.thread.name);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.state);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.cpu);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.cpuTime);
            ImGui::TableNextColumn();
            UI::Widgets::cellText(row.nice);
        }
    }
    ImGui::EndTable();
}

} // namespace App
