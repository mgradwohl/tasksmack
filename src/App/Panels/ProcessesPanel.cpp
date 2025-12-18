#include "ProcessesPanel.h"

#include "Platform/Factory.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace App
{

ProcessesPanel::ProcessesPanel() : Panel("Processes")
{
}

ProcessesPanel::~ProcessesPanel()
{
    // Stop and clear sampler before destruction to prevent callback use-after-free.
    // The callback captures 'this', so we must ensure the sampler thread is stopped
    // and the callback is cleared before any member destruction.
    if (m_Sampler)
    {
        m_Sampler->stop();
        m_Sampler->setCallback(nullptr); // Clear callback to prevent races
        m_Sampler.reset();
    }
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

    // Search bar
    const auto& theme = UI::Theme::get();
    ImGui::SetNextItemWidth(200.0F);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, theme.scheme().statusRunning);
    if (ImGui::InputTextWithHint("##search", "Filter by name...", m_SearchBuffer.data(), m_SearchBuffer.size()))
    {
        // Input changed - filter will be applied below
    }
    ImGui::PopStyleColor();

    // Clear button
    ImGui::SameLine();
    if (m_SearchBuffer[0] != '\0')
    {
        if (ImGui::SmallButton("X"))
        {
            m_SearchBuffer.fill('\0');
        }
    }

    // Filter snapshots based on search
    std::string_view searchTerm(m_SearchBuffer.data());
    std::vector<size_t> filteredIndices;
    filteredIndices.reserve(currentSnapshots.size());

    for (size_t i = 0; i < currentSnapshots.size(); ++i)
    {
        if (searchTerm.empty())
        {
            filteredIndices.push_back(i);
        }
        else
        {
            // Case-insensitive search in process name
            const auto& name = currentSnapshots[i].name;
            bool found = false;
            if (name.size() >= searchTerm.size())
            {
                for (size_t j = 0; j <= name.size() - searchTerm.size(); ++j)
                {
                    bool match = true;
                    for (size_t k = 0; k < searchTerm.size(); ++k)
                    {
                        if (std::tolower(static_cast<unsigned char>(name[j + k])) !=
                            std::tolower(static_cast<unsigned char>(searchTerm[k])))
                        {
                            match = false;
                            break;
                        }
                    }
                    if (match)
                    {
                        found = true;
                        break;
                    }
                }
            }
            if (found)
            {
                filteredIndices.push_back(i);
            }
        }
    }

    // Process count with state summary (filtered/total)
    ImGui::SameLine();

    // Count processes by state
    size_t runningCount = 0;
    for (const auto& proc : currentSnapshots)
    {
        if (proc.displayState == "Running")
        {
            ++runningCount;
        }
    }

    char summaryBuf[64];
    if (searchTerm.empty())
    {
        snprintf(summaryBuf, sizeof(summaryBuf), "%zu processes, %zu running", currentSnapshots.size(), runningCount);
    }
    else
    {
        snprintf(summaryBuf, sizeof(summaryBuf), "%zu / %zu processes", filteredIndices.size(), currentSnapshots.size());
    }

    const float rightEdgeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float textW = ImGui::CalcTextSize(summaryBuf).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdgeX - textW));
    ImGui::TextUnformatted(summaryBuf);

    ImGui::Separator();

    if (ImGui::BeginTable("ProcessTable",
                          10,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Hideable))
    {
        ImGui::TableSetupScrollFreeze(0, 1); // Freeze header row
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 60.0F);
        ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableSetupColumn("CPU %",
                                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed |
                                    ImGuiTableColumnFlags_PreferSortDescending,
                                55.0F);
        ImGui::TableSetupColumn("MEM %", ImGuiTableColumnFlags_WidthFixed, 55.0F);
        ImGui::TableSetupColumn("VIRT", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 80.0F);
        ImGui::TableSetupColumn("RES", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableSetupColumn("TIME+", ImGuiTableColumnFlags_WidthFixed, 80.0F);
        ImGui::TableSetupColumn("S", ImGuiTableColumnFlags_WidthFixed, 25.0F); // State (single char)
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 120.0F);
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // Handle sorting on filtered indices
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsCount > 0)
            {
                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                std::ranges::sort(filteredIndices,
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
                                      case 1: // User
                                          return compare(procA.user, procB.user);
                                      case 2: // CPU %
                                          return compare(procA.cpuPercent, procB.cpuPercent);
                                      case 3: // MEM %
                                          return compare(procA.memoryPercent, procB.memoryPercent);
                                      case 4: // VIRT
                                          return compare(procA.virtualBytes, procB.virtualBytes);
                                      case 5: // RES
                                          return compare(procA.memoryBytes, procB.memoryBytes);
                                      case 6: // TIME+
                                          return compare(procA.cpuTimeSeconds, procB.cpuTimeSeconds);
                                      case 7: // S (State)
                                          return compare(procA.displayState, procB.displayState);
                                      case 8: // Name
                                          return compare(procA.name, procB.name);
                                      case 9: // Command
                                          return compare(procA.command, procB.command);
                                      default:
                                          return false;
                                      }
                                  });
            }
        }

        // Render process rows in sorted order
        for (size_t idx : filteredIndices)
        {
            const auto& proc = currentSnapshots[idx];

            ImGui::TableNextRow();

            // Column 0: PID (selectable)
            ImGui::TableNextColumn();
            const bool isSelected = (m_SelectedPid == proc.pid);
            char label[32];
            snprintf(label, sizeof(label), "%d", proc.pid);

            if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                m_SelectedPid = proc.pid;
            }

            // Column 1: User
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(proc.user.c_str());

            // Column 2: CPU%
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", proc.cpuPercent);

            // Column 3: MEM%
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", proc.memoryPercent);

            // Column 4: VIRT (virtual memory)
            ImGui::TableNextColumn();
            {
                double virtMB = static_cast<double>(proc.virtualBytes) / (1024.0 * 1024.0);
                if (virtMB >= 1024.0)
                {
                    ImGui::Text("%.1fG", virtMB / 1024.0);
                }
                else if (virtMB >= 1.0)
                {
                    ImGui::Text("%.0fM", virtMB);
                }
                else
                {
                    ImGui::Text("%.0fK", static_cast<double>(proc.virtualBytes) / 1024.0);
                }
            }

            // Column 5: RES (resident memory)
            ImGui::TableNextColumn();
            {
                double resMB = static_cast<double>(proc.memoryBytes) / (1024.0 * 1024.0);
                if (resMB >= 1024.0)
                {
                    ImGui::Text("%.1fG", resMB / 1024.0);
                }
                else if (resMB >= 1.0)
                {
                    ImGui::Text("%.0fM", resMB);
                }
                else
                {
                    ImGui::Text("%.0fK", static_cast<double>(proc.memoryBytes) / 1024.0);
                }
            }

            // Column 6: TIME+ (CPU time as H:MM:SS.cc or MM:SS.cc)
            ImGui::TableNextColumn();
            {
                auto totalSeconds = static_cast<int>(proc.cpuTimeSeconds);
                int hours = totalSeconds / 3600;
                int minutes = (totalSeconds % 3600) / 60;
                int seconds = totalSeconds % 60;
                int centiseconds = static_cast<int>((proc.cpuTimeSeconds - static_cast<double>(totalSeconds)) * 100.0);

                if (hours > 0)
                {
                    ImGui::Text("%d:%02d:%02d.%02d", hours, minutes, seconds, centiseconds);
                }
                else
                {
                    ImGui::Text("%d:%02d.%02d", minutes, seconds, centiseconds);
                }
            }

            // Column 7: S (state as single char with color)
            ImGui::TableNextColumn();
            {
                char stateChar = proc.displayState.empty() ? '?' : proc.displayState[0];

                // Choose color based on process state
                ImVec4 stateColor = theme.scheme().textMuted; // Default
                if (proc.displayState == "Running")
                {
                    stateColor = theme.scheme().statusRunning;
                }
                else if (proc.displayState == "Sleeping")
                {
                    stateColor = theme.scheme().statusSleeping;
                }
                else if (proc.displayState == "Disk Sleep")
                {
                    stateColor = theme.scheme().statusDiskSleep;
                }
                else if (proc.displayState == "Zombie")
                {
                    stateColor = theme.scheme().statusZombie;
                }
                else if (proc.displayState == "Stopped" || proc.displayState == "Tracing")
                {
                    stateColor = theme.scheme().statusStopped;
                }
                else if (proc.displayState == "Idle")
                {
                    stateColor = theme.scheme().statusIdle;
                }

                ImGui::TextColored(stateColor, "%c", stateChar);
            }

            // Column 8: Name
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(proc.name.c_str());

            // Column 9: Command
            ImGui::TableNextColumn();
            if (!proc.command.empty())
            {
                ImGui::TextUnformatted(proc.command.c_str());
            }
            else
            {
                // Show name in brackets if no command line available
                ImGui::Text("[%s]", proc.name.c_str());
            }
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
