#include "RenderMetrics.h"

// clang-format off
// imgui_stdlib.h has to follow imgui.h (it extends ImGui's API for std::string) and belongs with
// the third-party group, but clang-format's IncludeCategories sort <misc/cpp/...> into the
// POSIX/C bucket below the standard library. Same guard as src/App/Panels/ProcessesPanel.cpp.
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
// clang-format on

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

    // Draw calls and command lists are read from the values UILayer published after
    // ImGui::Render(), NOT from ImGui::GetDrawData() here. ImDrawData is only valid between
    // Render() and the next NewFrame() -- NewFrame() sets DrawDataP.Valid = false and
    // GetDrawData() then returns null -- and this overlay draws during the build phase, so
    // reading it here reported a confident 0 on every frame (see #907).
    //
    // The two counts are distinct: ImDrawData::CmdListsCount is the number of ImDrawList objects
    // (roughly one per ImGui window), while each list's own CmdBuffer holds one ImDrawCmd per real
    // draw call, so the true draw-call count is the sum over every list's CmdBuffer (see #875).
    ImGui::Text("Total: %d vertices, %d indices, %d draw calls (%d command lists)",
                io.MetricsRenderVertices,
                io.MetricsRenderIndices,
                drawCalls(),
                commandLists());

    int chartVertices = 0;
    double chartMicros = 0.0;
    for (const auto& sample : m_LastFrame)
    {
        chartVertices += sample.vertices;
        chartMicros += sample.micros;
    }
    ImGui::Text("Charts: %d vertices, %.0f us CPU (%zu charts)", chartVertices, chartMicros, m_LastFrame.size());

    // Free-form label identifying what's currently being profiled (e.g. "idle",
    // "process-list-1000-rows"), included in every exported CSV row so pasted exports from
    // different capture sessions can be told apart later. Synced from the persisted value once;
    // edits below flow back into m_Scenario immediately so a mid-session export picks them up.
    static std::string scenarioInput = scenario();
    ImGui::SetNextItemWidth(200.0F);
    if (ImGui::InputTextWithHint("Scenario", "e.g. idle, resize, 1000-processes", &scenarioInput))
    {
        setScenario(scenarioInput);
    }

    if (ImGui::Button("Copy CSV"))
    {
        ImGui::SetClipboardText(toCsv().c_str());
    }

    ImGui::Separator();

    constexpr ImGuiTableFlags TABLE_FLAGS =
        ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##RenderMetricsTable", 3, TABLE_FLAGS))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Chart", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Vertices", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 80.0F);
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
            ImGui::Text("%.1f", sample->micros);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

} // namespace UI
