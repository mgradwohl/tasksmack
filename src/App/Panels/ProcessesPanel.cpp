#include "ProcessesPanel.h"

#include "Platform/Factory.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include <imgui.h>

namespace App
{

ProcessesPanel::ProcessesPanel() : Panel("Processes")
{
}

ProcessesPanel::~ProcessesPanel()
{
    onDetach();
}

void ProcessesPanel::onAttach()
{
    // Create process model (no probe - we'll feed it from sampler)
    m_ProcessModel = std::make_unique<Domain::ProcessModel>(nullptr);

    // Create and start background sampler
    m_Sampler =
        std::make_unique<Domain::BackgroundSampler>(Platform::makeProcessProbe(), Domain::SamplerConfig{std::chrono::milliseconds(1000)});

    // Set callback to update model when new data arrives
    m_Sampler->setCallback([this](const std::vector<Platform::ProcessCounters>& counters, uint64_t totalCpuTime)
                           { m_ProcessModel->updateFromCounters(counters, totalCpuTime); });

    m_Sampler->start();

    spdlog::info("ProcessesPanel: initialized with background sampling");
}

void ProcessesPanel::onDetach()
{
    if (m_Sampler)
    {
        m_Sampler->stop();
        m_Sampler.reset();
    }
    m_ProcessModel.reset();
}

void ProcessesPanel::onUpdate(float /* deltaTime */)
{
    // No longer needed - background sampler handles refresh
}

void ProcessesPanel::render(bool* open)
{
    if (!ImGui::Begin("Processes", open))
    {
        ImGui::End();
        return;
    }

    if (!m_ProcessModel)
    {
        ImGui::TextColored(ImVec4(1.0F, 0.3F, 0.3F, 1.0F), "Process model not initialized");
        ImGui::End();
        return;
    }

    // Get thread-safe copy of snapshots
    auto currentSnapshots = m_ProcessModel->snapshots();
    ImGui::Text("%zu processes", currentSnapshots.size());

    // Show sampler status
    if (m_Sampler && m_Sampler->isRunning())
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3F, 1.0F, 0.3F, 1.0F), "(sampling)");
    }

    ImGui::Separator();

    if (ImGui::BeginTable("ProcessTable",
                          5,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupScrollFreeze(0, 1); // Freeze header row
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 60.0F);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("CPU %",
                                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed |
                                    ImGuiTableColumnFlags_PreferSortDescending,
                                70.0F);
        ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableHeadersRow();

        // Create sorted indices
        std::vector<size_t> sortedIndices(currentSnapshots.size());
        for (size_t i = 0; i < currentSnapshots.size(); ++i)
        {
            sortedIndices[i] = i;
        }

        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsCount > 0)
            {
                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                std::sort(sortedIndices.begin(),
                          sortedIndices.end(),
                          [&currentSnapshots, &spec, ascending](size_t a, size_t b)
                          {
                              const auto& procA = currentSnapshots[a];
                              const auto& procB = currentSnapshots[b];

                              int cmp = 0;
                              switch (spec.ColumnIndex)
                              {
                              case 0: // PID
                                  cmp = (procA.pid < procB.pid) ? -1 : ((procA.pid > procB.pid) ? 1 : 0);
                                  break;
                              case 1: // Name
                                  cmp = procA.name.compare(procB.name);
                                  break;
                              case 2: // CPU %
                                  cmp = (procA.cpuPercent < procB.cpuPercent) ? -1 : ((procA.cpuPercent > procB.cpuPercent) ? 1 : 0);
                                  break;
                              case 3: // Memory
                                  cmp = (procA.memoryBytes < procB.memoryBytes) ? -1 : ((procA.memoryBytes > procB.memoryBytes) ? 1 : 0);
                                  break;
                              case 4: // Status
                                  cmp = procA.displayState.compare(procB.displayState);
                                  break;
                              default:
                                  break;
                              }

                              return ascending ? (cmp < 0) : (cmp > 0);
                          });
            }
        }

        // Render process rows in sorted order
        for (size_t idx : sortedIndices)
        {
            const auto& proc = currentSnapshots[idx];

            ImGui::TableNextRow();

            // Make row selectable
            ImGui::TableNextColumn();
            const bool isSelected = (m_SelectedPid == proc.pid);
            char label[32];
            snprintf(label, sizeof(label), "%d", proc.pid);

            if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                m_SelectedPid = proc.pid;
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(proc.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.1f%%", proc.cpuPercent);
            ImGui::TableNextColumn();

            // Format memory (KB, MB, or GB)
            double memoryMB = static_cast<double>(proc.memoryBytes) / (1024.0 * 1024.0);
            if (memoryMB >= 1024.0)
            {
                ImGui::Text("%.1f GB", memoryMB / 1024.0);
            }
            else if (memoryMB >= 1.0)
            {
                ImGui::Text("%.1f MB", memoryMB);
            }
            else
            {
                ImGui::Text("%.0f KB", static_cast<double>(proc.memoryBytes) / 1024.0);
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(proc.displayState.c_str());
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

size_t ProcessesPanel::processCount() const
{
    return m_ProcessModel ? m_ProcessModel->processCount() : 0;
}

std::vector<Domain::ProcessSnapshot> ProcessesPanel::snapshots() const
{
    if (m_ProcessModel)
    {
        return m_ProcessModel->snapshots();
    }
    return {};
}

} // namespace App
