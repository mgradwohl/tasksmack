#include "ProcessesPanel.h"

#include "Platform/Factory.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace App
{

ProcessesPanel::ProcessesPanel() : Panel("Processes")
{
}

ProcessesPanel::~ProcessesPanel()
{
    // Note: onDetach() should be called by the layer stack, not destructor
    // to avoid virtual dispatch during destruction
    m_Sampler.reset();
    m_ProcessModel.reset();
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
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textError, "Process model not initialized");
        ImGui::End();
        return;
    }

    // Get thread-safe copy of snapshots
    auto currentSnapshots = m_ProcessModel->snapshots();
    ImGui::Text("%zu processes", currentSnapshots.size());

    // Show sampler status
    if (m_Sampler && m_Sampler->isRunning())
    {
        const auto& theme = UI::Theme::get();
        ImGui::SameLine();
        ImGui::TextColored(theme.scheme().statusRunning, "(sampling)");
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

                std::ranges::sort(sortedIndices,
                                  [&currentSnapshots, &spec, ascending](size_t a, size_t b)
                                  {
                                      const auto& procA = currentSnapshots[a];
                                      const auto& procB = currentSnapshots[b];

                                      auto compare = [ascending](auto lhs, auto rhs) -> bool
                                      {
                                          return ascending ? (lhs < rhs) : (rhs < lhs);
                                      };

                                      switch (spec.ColumnIndex)
                                      {
                                      case 0: // PID
                                          return compare(procA.pid, procB.pid);
                                      case 1: // Name
                                          return compare(procA.name, procB.name);
                                      case 2: // CPU %
                                          return compare(procA.cpuPercent, procB.cpuPercent);
                                      case 3: // Memory
                                          return compare(procA.memoryBytes, procB.memoryBytes);
                                      case 4: // Status
                                          return compare(procA.displayState, procB.displayState);
                                      default:
                                          return false;
                                      }
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
