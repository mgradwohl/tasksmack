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
#include <string_view>

namespace UI::Widgets
{

namespace Detail
{
/// Default CellIdFn tag for renderChartGrid: "use the loop index as the ImGui ID", correct for
/// collections whose index already *is* the item's identity (e.g. a CPU core number) and where
/// item order never changes at runtime.
struct UseIndexAsId
{};
} // namespace Detail

/// Render a grid of itemCount bordered cells sized by computeChartGridLayout(config), calling
/// renderCell(index, cellWidth, cellHeight) for each cell in row-major order. Cells past
/// itemCount in the final row (e.g. 16 items in a 4x5 grid) are skipped, not rendered blank.
///
/// renderCell runs inside an active ImGui::BeginChild sized (cellWidth, cellHeight) --
/// cellWidth/cellHeight are the child's *usable content* dimensions (measured via
/// GetContentRegionAvail() right after BeginChild, not the outer size passed to it), so callers
/// that size content to exactly fill the cell (e.g. a HistoryChartConfig::height) don't overflow
/// it and trigger a scrollbar: a bordered child reserves WindowPadding on all sides from its
/// declared outer size, so content sized to that outer size directly is WindowPadding.y*2 too
/// tall -- see #823 review (reported as a visible scrollbar on every chart cell).
///
/// Cell IDs default to ImGui::PushID(index) (no heap allocation) -- correct as long as index
/// *is* the item's identity and never reorders (true for CPU cores). For a collection whose
/// membership/order can change at runtime (e.g. disks, which can be unplugged mid-session,
/// shifting later indices), pass cellId so ImGui/ImPlot per-widget state (pan/zoom, RenderMetrics
/// entries) stays attached to the same logical item instead of "jumping" to whatever now
/// occupies that index -- see #823 review.
///
/// A template (not std::function) so passing a capturing lambda doesn't force a type-erased
/// wrapper allocation on this resize-time-hot path.
///
/// itemCount is the single source of truth for the grid's item count: config.itemCount is
/// overwritten from it before computing the layout, so a caller can't accidentally desync the
/// two (e.g. by updating one but not the other on a later edit).
template<typename RenderCellFn, typename CellIdFn = Detail::UseIndexAsId>
    requires std::invocable<RenderCellFn, size_t, float, float>
inline void renderChartGrid(const char* tableId, size_t itemCount, ChartGridConfig config, RenderCellFn renderCell, CellIdFn cellId = {})
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

    const auto& theme = UI::Theme::get();
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

            if constexpr (std::same_as<CellIdFn, Detail::UseIndexAsId>)
            {
                ImGui::PushID(UI::Format::checkedCount(index));
            }
            else
            {
                // Keep the actual return value alive (not just a string_view over it) for the
                // duration of PushID: if cellId returns a std::string by value, converting it
                // straight to string_view would dangle once the temporary is destroyed at the
                // end of this statement (see #823 review).
                const auto id = cellId(index);
                ImGui::PushID(id.data(), id.data() + id.size());
            }
            ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.scheme().childBg);
            ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().separator);
            if (ImGui::BeginChild("GridCell", ImVec2(-FLT_MIN, grid.cellHeight), ImGuiChildFlags_Borders))
            {
                const ImVec2 innerAvail = ImGui::GetContentRegionAvail();
                renderCell(index, innerAvail.x, innerAvail.y);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

} // namespace UI::Widgets
