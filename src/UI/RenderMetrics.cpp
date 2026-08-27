#include "RenderMetrics.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

namespace UI
{

void RenderMetrics::renderOverlay(bool* open)
{
    if (open == nullptr || !*open)
    {
        setEnabled(false);
        return;
    }
    setEnabled(true);

    ImGui::SetNextWindowSize(ImVec2(460.0F, 340.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Render Metrics", open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    // Cast to double: printf-style varargs promote float anyway; make it explicit for -Wdouble-promotion.
    const double framerate = static_cast<double>(std::max(io.Framerate, 1.0F));
    ImGui::Text("Frame: %.2f ms (%.0f FPS)", 1000.0 / framerate, framerate);
    ImGui::Text("Total: %d vertices, %d indices, %d draw calls",
                io.MetricsRenderVertices,
                io.MetricsRenderIndices,
                ImGui::GetDrawData() != nullptr ? ImGui::GetDrawData()->CmdListsCount : 0);

    int chartVertices = 0;
    int chartIndices = 0;
    double chartMicros = 0.0;
    for (const auto& sample : m_LastFrame)
    {
        chartVertices += sample.vertices;
        chartIndices += sample.indices;
        chartMicros += sample.micros;
    }
    ImGui::Text("Charts: %d vertices, %d indices, %.0f us CPU (%zu charts)", chartVertices, chartIndices, chartMicros, m_LastFrame.size());

    if (ImGui::Button("Copy CSV"))
    {
        ImGui::SetClipboardText(toCsv().c_str());
    }

    ImGui::Separator();

    constexpr ImGuiTableFlags TABLE_FLAGS =
        ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##RenderMetricsTable", 4, TABLE_FLAGS))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Chart", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Vertices", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 80.0F);
        ImGui::TableSetupColumn("Indices", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableSetupColumn("CPU (us)", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableHeadersRow();

        // Copy for display sorting so the recorded order stays stable for CSV export.
        std::vector<const ChartRenderSample*> sorted;
        sorted.reserve(m_LastFrame.size());
        for (const auto& sample : m_LastFrame)
        {
            sorted.push_back(&sample);
        }

        if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsCount > 0)
        {
            const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
            const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;
            std::ranges::sort(sorted,
                              [&spec, ascending](const ChartRenderSample* a, const ChartRenderSample* b)
                              {
                                  const auto ordered = [ascending](const auto& lhs, const auto& rhs)
                                  {
                                      return ascending ? lhs < rhs : lhs > rhs;
                                  };
                                  switch (spec.ColumnIndex)
                                  {
                                  case 1:
                                      return ordered(a->vertices, b->vertices);
                                  case 2:
                                      return ordered(a->indices, b->indices);
                                  case 3:
                                      return ordered(a->micros, b->micros);
                                  default:
                                      return ordered(a->id, b->id);
                                  }
                              });
        }

        for (const ChartRenderSample* sample : sorted)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(sample->id.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", sample->vertices);
            ImGui::TableNextColumn();
            ImGui::Text("%d", sample->indices);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", sample->micros);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace UI
