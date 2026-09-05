#pragma once

// ImGui rendering helper built on UI/ChartGridLayout.h's pure sizing math. Owns the
// BeginTable/BeginChild scaffolding that CpuCoresSection.cpp and StorageSection.cpp used to
// duplicate for their "grid of N small charts" panels (per-core CPU, per-disk I/O), so a new
// consumer only has to supply a per-cell render callback.

#include "UI/ChartGridLayout.h"
#include "UI/Format.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <cfloat>
#include <concepts>
#include <cstddef>

namespace UI::Widgets
{

/// Render a grid of itemCount bordered cells sized by computeChartGridLayout(config), calling
/// renderCell(index, cellWidth, cellHeight) for each cell in row-major order. Cells past
/// itemCount in the final row (e.g. 16 items in a 4x5 grid) are skipped, not rendered blank.
///
/// renderCell runs inside an active ImGui::BeginChild of size (cellWidth, cellHeight) --
/// cellWidth is informational (the child already fills the stretched table column via
/// ImVec2(-FLT_MIN, ...)); cellHeight is the value callers need to size their own content
/// (e.g. a HistoryChartConfig::height) so it fills the cell instead of using a fixed constant.
///
/// Cell IDs use ImGui::PushID(index) rather than a formatted per-cell string, avoiding a
/// heap allocation per cell per frame -- relevant here since this can run every frame for
/// dozens of cells (CPU cores, disks) while the window is being resized.
///
/// A template (not std::function) so passing a capturing lambda doesn't force a type-erased
/// wrapper allocation on this resize-time-hot path.
///
/// itemCount is the single source of truth for the grid's item count: config.itemCount is
/// overwritten from it before computing the layout, so a caller can't accidentally desync the
/// two (e.g. by updating one but not the other on a later edit).
template<typename RenderCellFn>
    requires std::invocable<RenderCellFn, size_t, float, float>
inline void renderChartGrid(const char* tableId, size_t itemCount, ChartGridConfig config, RenderCellFn renderCell)
{
    if (itemCount == 0)
    {
        return;
    }

    config.itemCount = itemCount;
    const ChartGridDimensions grid = computeChartGridLayout(config);
    const int columnsInt = UI::Format::checkedCount(grid.columns);

    if (!ImGui::BeginTable(tableId, columnsInt, ImGuiTableFlags_SizingStretchSame))
    {
        return;
    }

    auto& theme = UI::Theme::get();
    for (size_t row = 0; row < grid.rows; ++row)
    {
        ImGui::TableNextRow();
        for (size_t col = 0; col < grid.columns; ++col)
        {
            const size_t index = (row * grid.columns) + col;
            ImGui::TableNextColumn();
            if (index >= itemCount)
            {
                continue;
            }

            ImGui::PushID(static_cast<int>(index));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.scheme().childBg);
            ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().separator);
            if (ImGui::BeginChild("GridCell", ImVec2(-FLT_MIN, grid.cellHeight), ImGuiChildFlags_Borders))
            {
                renderCell(index, grid.cellWidth, grid.cellHeight);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

} // namespace UI::Widgets
