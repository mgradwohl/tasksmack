#include "ProcessesPanel.h"

#include "App/ProcessColumnConfig.h"
#include "App/UserConfig.h"
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
    // Load column settings from user config
    m_ColumnSettings = UserConfig::get().settings().processColumns;

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
    // Save column settings to user config
    UserConfig::get().settings().processColumns = m_ColumnSettings;

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

int ProcessesPanel::visibleColumnCount() const
{
    int count = 0;
    for (size_t i = 0; i < static_cast<size_t>(ProcessColumn::Count); ++i)
    {
        if (m_ColumnSettings.isVisible(static_cast<ProcessColumn>(i)))
        {
            ++count;
        }
    }
    return count;
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

    // Always create all columns with stable IDs (using enum value as ID)
    // Hidden columns use ImGuiTableColumnFlags_Disabled
    constexpr int totalColumns = static_cast<int>(ProcessColumn::Count);

    if (ImGui::BeginTable("ProcessTable",
                          totalColumns,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupScrollFreeze(0, 1); // Freeze header row

        // Setup ALL columns with stable IDs - use enum value as user_id for stable identification
        for (size_t i = 0; i < static_cast<size_t>(ProcessColumn::Count); ++i)
        {
            auto col = static_cast<ProcessColumn>(i);
            const auto info = getColumnInfo(col);
            ImGuiTableColumnFlags flags = ImGuiTableColumnFlags_None;

            // Set default visibility from settings (ImGui will manage the actual state)
            if (!m_ColumnSettings.isVisible(col))
            {
                flags |= ImGuiTableColumnFlags_DefaultHide;
            }

            // PID and Name columns cannot be hidden
            if (!info.canHide)
            {
                flags |= ImGuiTableColumnFlags_NoHide;
            }

            // Default sort on CPU%
            if (col == ProcessColumn::CpuPercent)
            {
                flags |= ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending;
            }

            // Command column stretches, others have initial width (all can be resized/auto-fitted)
            if (info.defaultWidth > 0.0F)
            {
                // Use enum value as user_id for stable column identification
                ImGui::TableSetupColumn(std::string(info.name).c_str(), flags, info.defaultWidth, static_cast<ImGuiID>(col));
            }
            else
            {
                flags |= ImGuiTableColumnFlags_WidthStretch;
                ImGui::TableSetupColumn(std::string(info.name).c_str(), flags, 0.0F, static_cast<ImGuiID>(col));
            }
        }

        ImGui::TableHeadersRow();

        // Handle sorting
        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsCount > 0)
            {
                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                const bool ascending = (spec.SortDirection == ImGuiSortDirection_Ascending);

                // Use ColumnUserID to get ProcessColumn (we set user_id = enum value)
                auto sortCol = static_cast<ProcessColumn>(spec.ColumnUserID);

                std::ranges::sort(filteredIndices,
                                  [&currentSnapshots, sortCol, ascending](size_t a, size_t b)
                                  {
                                      const auto& procA = currentSnapshots[a];
                                      const auto& procB = currentSnapshots[b];

                                      auto compare = [ascending](auto lhs, auto rhs) -> bool
                                      {
                                          return ascending ? (lhs < rhs) : (rhs < lhs);
                                      };

                                      switch (sortCol)
                                      {
                                      case ProcessColumn::PID:
                                          return compare(procA.pid, procB.pid);
                                      case ProcessColumn::User:
                                          return compare(procA.user, procB.user);
                                      case ProcessColumn::CpuPercent:
                                          return compare(procA.cpuPercent, procB.cpuPercent);
                                      case ProcessColumn::MemPercent:
                                          return compare(procA.memoryPercent, procB.memoryPercent);
                                      case ProcessColumn::Virtual:
                                          return compare(procA.virtualBytes, procB.virtualBytes);
                                      case ProcessColumn::Resident:
                                          return compare(procA.memoryBytes, procB.memoryBytes);
                                      case ProcessColumn::Shared:
                                          return compare(procA.sharedBytes, procB.sharedBytes);
                                      case ProcessColumn::CpuTime:
                                          return compare(procA.cpuTimeSeconds, procB.cpuTimeSeconds);
                                      case ProcessColumn::State:
                                          return compare(procA.displayState, procB.displayState);
                                      case ProcessColumn::Name:
                                          return compare(procA.name, procB.name);
                                      case ProcessColumn::PPID:
                                          return compare(procA.parentPid, procB.parentPid);
                                      case ProcessColumn::Nice:
                                          return compare(procA.nice, procB.nice);
                                      case ProcessColumn::Threads:
                                          return compare(procA.threadCount, procB.threadCount);
                                      case ProcessColumn::Command:
                                          return compare(procA.command, procB.command);
                                      default:
                                          return false;
                                      }
                                  });
            }
        }

        // Render process rows
        for (size_t idx : filteredIndices)
        {
            const auto& proc = currentSnapshots[idx];

            ImGui::TableNextRow();

            // Render all columns - ImGui handles hidden columns automatically
            for (size_t colIdx = 0; colIdx < static_cast<size_t>(ProcessColumn::Count); ++colIdx)
            {
                auto col = static_cast<ProcessColumn>(colIdx);
                if (!ImGui::TableSetColumnIndex(static_cast<int>(colIdx)))
                {
                    continue; // Column is hidden or clipped
                }

                // PID column is always the selectable
                if (col == ProcessColumn::PID)
                {
                    const bool isSelected = (m_SelectedPid == proc.pid);
                    char label[32];
                    snprintf(label, sizeof(label), "%d", proc.pid);

                    if (ImGui::Selectable(label, isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    {
                        m_SelectedPid = proc.pid;
                    }
                    continue;
                }

                switch (col)
                {
                case ProcessColumn::User:
                    ImGui::TextUnformatted(proc.user.c_str());
                    break;

                case ProcessColumn::CpuPercent:
                    ImGui::Text("%.1f", proc.cpuPercent);
                    break;

                case ProcessColumn::MemPercent:
                    ImGui::Text("%.1f", proc.memoryPercent);
                    break;

                case ProcessColumn::Virtual:
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
                    break;
                }

                case ProcessColumn::Resident:
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
                    break;
                }

                case ProcessColumn::Shared:
                {
                    double shrMB = static_cast<double>(proc.sharedBytes) / (1024.0 * 1024.0);
                    if (shrMB >= 1024.0)
                    {
                        ImGui::Text("%.1fG", shrMB / 1024.0);
                    }
                    else if (shrMB >= 1.0)
                    {
                        ImGui::Text("%.0fM", shrMB);
                    }
                    else
                    {
                        ImGui::Text("%.0fK", static_cast<double>(proc.sharedBytes) / 1024.0);
                    }
                    break;
                }

                case ProcessColumn::CpuTime:
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
                    break;
                }

                case ProcessColumn::State:
                {
                    char stateChar = proc.displayState.empty() ? '?' : proc.displayState[0];
                    const auto& scheme = UI::Theme::get().scheme();

                    // Color based on process state
                    ImVec4 stateColor;
                    switch (stateChar)
                    {
                    case 'R': // Running
                        stateColor = scheme.statusRunning;
                        break;
                    case 'S': // Sleeping (interruptible)
                        stateColor = scheme.statusSleeping;
                        break;
                    case 'D': // Disk sleep (uninterruptible)
                        stateColor = scheme.statusDiskSleep;
                        break;
                    case 'Z': // Zombie
                        stateColor = scheme.statusZombie;
                        break;
                    case 'T': // Stopped/Traced
                    case 't': // Tracing stop
                        stateColor = scheme.statusStopped;
                        break;
                    case 'I': // Idle kernel thread
                        stateColor = scheme.statusIdle;
                        break;
                    default:
                        stateColor = scheme.statusSleeping; // Default to muted
                        break;
                    }

                    ImGui::PushStyleColor(ImGuiCol_Text, stateColor);
                    ImGui::Text("%c", stateChar);
                    ImGui::PopStyleColor();
                    break;
                }

                case ProcessColumn::Name:
                    ImGui::TextUnformatted(proc.name.c_str());
                    break;

                case ProcessColumn::PPID:
                    ImGui::Text("%d", proc.parentPid);
                    break;

                case ProcessColumn::Nice:
                    ImGui::Text("%d", proc.nice);
                    break;

                case ProcessColumn::Threads:
                    if (proc.threadCount > 0)
                    {
                        ImGui::Text("%d", proc.threadCount);
                    }
                    else
                    {
                        ImGui::TextUnformatted("-");
                    }
                    break;

                case ProcessColumn::Command:
                    if (!proc.command.empty())
                    {
                        ImGui::TextUnformatted(proc.command.c_str());
                    }
                    else
                    {
                        // Show name in brackets if no command line available
                        ImGui::Text("[%s]", proc.name.c_str());
                    }
                    break;

                default:
                    break;
                }
            }
        }

        // Sync column visibility from ImGui back to our settings
        // This captures changes made via the right-click context menu
        bool settingsChanged = false;
        for (size_t i = 0; i < static_cast<size_t>(ProcessColumn::Count); ++i)
        {
            auto col = static_cast<ProcessColumn>(i);
            bool isEnabled = (ImGui::TableGetColumnFlags(static_cast<int>(i)) & ImGuiTableColumnFlags_IsEnabled) != 0;
            if (m_ColumnSettings.isVisible(col) != isEnabled)
            {
                m_ColumnSettings.setVisible(col, isEnabled);
                settingsChanged = true;
            }
        }
        if (settingsChanged)
        {
            UserConfig::get().settings().processColumns = m_ColumnSettings;
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
