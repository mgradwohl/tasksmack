#include "ProcessesPanel.h"

#include "App/Panel.h"
#include "App/ProcessColumnConfig.h"
#include "App/UserConfig.h"
#include "Domain/ProcessModel.h"
#include "Platform/Factory.h"
#include "UI/Format.h"
#include "UI/Numeric.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace App
{

namespace
{

constexpr float TREE_INDENT_WIDTH = 16.0F; // Indent width per tree level in pixels

[[nodiscard]] auto lowerAscii(char ch) -> int
{
    // Safe/necessary: std::tolower is undefined for negative signed char values (except EOF).
    // Cast to unsigned char to avoid UB when char is signed.
    return std::tolower(static_cast<unsigned char>(ch));
}

[[nodiscard]] constexpr auto toImGuiId(ProcessColumn col) noexcept -> ImGuiID
{
    // Safe: ProcessColumn is a small uint8_t-backed enum; ImGuiID is a wider unsigned type.
    return ImGuiID{std::to_underlying(col)};
}

[[nodiscard]] auto columnFromUserId(ImGuiID id) -> std::optional<ProcessColumn>
{
    // Map back via known columns to avoid integer->enum casts.
    for (const ProcessColumn col : allProcessColumns())
    {
        if (toImGuiId(col) == id)
        {
            return col;
        }
    }

    return std::nullopt;
}

} // namespace

ProcessesPanel::ProcessesPanel() : Panel("Processes")
{
}

ProcessesPanel::~ProcessesPanel()
{
    m_ProcessModel.reset();
}

void ProcessesPanel::onAttach()
{
    // Load column settings from user config
    m_ColumnSettings = UserConfig::get().settings().processColumns;

    const int intervalMs = UserConfig::get().settings().refreshIntervalMs;
    m_RefreshInterval = std::chrono::milliseconds(intervalMs);
    m_RefreshAccumulatorSec = 0.0F;
    m_ForceRefresh = true;

    // Create process model with platform probe; refresh is driven by onUpdate().
    m_ProcessModel = std::make_unique<Domain::ProcessModel>(Platform::makeProcessProbe());

    // Initial population
    m_ProcessModel->refresh();
    m_ForceRefresh = false;

    spdlog::info("ProcessesPanel: initialized with main-loop-driven refresh");
}

void ProcessesPanel::setSamplingInterval(std::chrono::milliseconds interval)
{
    m_RefreshInterval = interval;
    m_RefreshAccumulatorSec = 0.0F;
    m_ForceRefresh = true;
}

void ProcessesPanel::requestRefresh()
{
    m_ForceRefresh = true;
}

void ProcessesPanel::onDetach()
{
    // Save column settings to user config
    UserConfig::get().settings().processColumns = m_ColumnSettings;
    m_ProcessModel.reset();
}

void ProcessesPanel::onUpdate(float deltaTime)
{
    if (!m_ProcessModel)
    {
        return;
    }

    m_RefreshAccumulatorSec += deltaTime;

    using SecondsF = std::chrono::duration<float>;
    const float intervalSec = std::chrono::duration_cast<SecondsF>(m_RefreshInterval).count();
    const bool intervalElapsed = (intervalSec > 0.0F) && (m_RefreshAccumulatorSec >= intervalSec);

    if (m_ForceRefresh || intervalElapsed)
    {
        m_ProcessModel->refresh();
        m_ForceRefresh = false;
        if (intervalSec > 0.0F)
        {
            // Keep remainder to avoid drift on low FPS.
            while (m_RefreshAccumulatorSec >= intervalSec)
            {
                m_RefreshAccumulatorSec -= intervalSec;
            }
        }
        else
        {
            m_RefreshAccumulatorSec = 0.0F;
        }
    }
}

int ProcessesPanel::visibleColumnCount() const
{
    int count = 0;
    for (const ProcessColumn col : allProcessColumns())
    {
        if (m_ColumnSettings.isVisible(col))
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
                        if (lowerAscii(name[j + k]) != lowerAscii(searchTerm[k]))
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
    ImGui::Separator();

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

    std::string summaryStr;
    if (searchTerm.empty())
    {
        summaryStr = std::format("{} processes, {} running", currentSnapshots.size(), runningCount);
    }
    else
    {
        summaryStr = std::format("{} / {} processes", filteredIndices.size(), currentSnapshots.size());
    }

    // get the width of the button before we create it
    const char* label = "XXXX View";
    const ImVec2 text = ImGui::CalcTextSize(label);
    const ImGuiStyle& style = ImGui::GetStyle();
    float buttonWidthPx = text.x + (style.FramePadding.x * 2.0f);

    const float rightEdgeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float textW = ImGui::CalcTextSize(summaryStr.c_str()).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightEdgeX - textW - buttonWidthPx - style.ItemSpacing.x));
    ImGui::TextUnformatted(summaryStr.c_str());

    // Tree view toggle button
    ImGui::SameLine();
    if (ImGui::Button(m_TreeViewEnabled ? "List View" : "Tree View"))
    {
        m_TreeViewEnabled = !m_TreeViewEnabled;
        if (m_TreeViewEnabled)
        {
            spdlog::info("Switched to tree view mode");
        }
        else
        {
            spdlog::info("Switched to flat list view mode");
        }
    }

    // Always create all columns with stable IDs (using enum value as ID)
    // Hidden columns use ImGuiTableColumnFlags_Disabled
    const int totalColumns = UI::Numeric::checkedCount(processColumnCount());

    if (ImGui::BeginTable("ProcessTable",
                          totalColumns,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupScrollFreeze(0, 1); // Freeze header row

        // Setup ALL columns with stable IDs - use enum value as user_id for stable identification
        for (const ProcessColumn col : allProcessColumns())
        {
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
                ImGui::TableSetupColumn(std::string(info.name).c_str(), flags, info.defaultWidth, toImGuiId(col));
            }
            else
            {
                flags |= ImGuiTableColumnFlags_WidthStretch;
                ImGui::TableSetupColumn(std::string(info.name).c_str(), flags, 0.0F, toImGuiId(col));
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
                const std::optional<ProcessColumn> sortColOpt = columnFromUserId(spec.ColumnUserID);
                if (!sortColOpt.has_value())
                {
                    sortSpecs->SpecsDirty = true;
                    ImGui::EndTable();
                    ImGui::End();
                    return;
                }

                const ProcessColumn sortCol = *sortColOpt;

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

        // Render process rows - tree view or flat list
        if (m_TreeViewEnabled)
        {
            // Build process tree
            const auto tree = buildProcessTree(currentSnapshots);

            // Render tree view
            renderTreeView(currentSnapshots, filteredIndices, tree);
        }
        else
        {
            // Render flat list
            for (size_t idx : filteredIndices)
            {
                const auto& proc = currentSnapshots[idx];
                renderProcessRow(proc, 0, false, false);
            }
        }

        // Sync column visibility from ImGui back to our settings
        // This captures changes made via the right-click context menu
        bool settingsChanged = false;
        int idx = 0;
        for (const ProcessColumn col : allProcessColumns())
        {
            const bool isEnabled = (ImGui::TableGetColumnFlags(idx) & ImGuiTableColumnFlags_IsEnabled) != 0;
            if (m_ColumnSettings.isVisible(col) != isEnabled)
            {
                m_ColumnSettings.setVisible(col, isEnabled);
                settingsChanged = true;
            }
            ++idx;
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

std::unordered_map<std::int32_t, std::vector<std::size_t>>
ProcessesPanel::buildProcessTree(const std::vector<Domain::ProcessSnapshot>& snapshots) const
{
    std::unordered_map<std::int32_t, std::vector<std::size_t>> tree;

    // Build parent -> children mapping
    for (std::size_t i = 0; i < snapshots.size(); ++i)
    {
        const auto& proc = snapshots[i];
        if (proc.parentPid > 0)
        {
            tree[proc.parentPid].push_back(i);
        }
    }

    return tree;
}

void ProcessesPanel::renderProcessRow(const Domain::ProcessSnapshot& proc, int depth, bool hasChildren, bool isExpanded)
{
    ImGui::TableNextRow();

    // Render all columns
    int colIdx = 0;
    for (const ProcessColumn col : allProcessColumns())
    {
        if (!ImGui::TableSetColumnIndex(colIdx))
        {
            ++colIdx;
            continue; // Column is hidden or clipped
        }
        ++colIdx;

        // PID column with tree indent and expand/collapse indicator
        if (col == ProcessColumn::PID)
        {
            const bool isSelected = (m_SelectedPid == proc.pid);

            // Indent for tree depth
            if (m_TreeViewEnabled && depth > 0)
            {
                const float indentWidth = TREE_INDENT_WIDTH * static_cast<float>(depth);
                ImGui::Indent(indentWidth);
            }

            // Tree expand/collapse button
            if (m_TreeViewEnabled && hasChildren)
            {
                const std::string buttonLabel = isExpanded ? "-" : "+";
                const std::string buttonId = std::format("{}##tree_btn_{}", buttonLabel, proc.pid);
                if (ImGui::SmallButton(buttonId.c_str()))
                {
                    // Toggle collapsed state
                    if (isExpanded)
                    {
                        m_CollapsedPids.insert(proc.pid);
                    }
                    else
                    {
                        m_CollapsedPids.erase(proc.pid);
                    }
                }
                ImGui::SameLine();
            }
            else if (m_TreeViewEnabled)
            {
                // Add spacing for processes without children
                ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0.0F));
                ImGui::SameLine();
            }

            const std::string label = std::format("{}", proc.pid);
            if (ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            {
                m_SelectedPid = proc.pid;
            }

            if (m_TreeViewEnabled && depth > 0)
            {
                const float indentWidth = TREE_INDENT_WIDTH * static_cast<float>(depth);
                ImGui::Unindent(indentWidth);
            }
            continue;
        }

        // Render other columns (same as before)
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
            const auto unit = UI::Format::unitForTotalBytes(proc.virtualBytes);
            const std::string text = UI::Format::formatBytesWithUnit(proc.virtualBytes, unit);
            ImGui::TextUnformatted(text.c_str());
            break;
        }

        case ProcessColumn::Resident:
        {
            const auto unit = UI::Format::unitForTotalBytes(proc.memoryBytes);
            const std::string text = UI::Format::formatBytesWithUnit(proc.memoryBytes, unit);
            ImGui::TextUnformatted(text.c_str());
            break;
        }

        case ProcessColumn::Shared:
        {
            const auto unit = UI::Format::unitForTotalBytes(proc.sharedBytes);
            const std::string text = UI::Format::formatBytesWithUnit(proc.sharedBytes, unit);
            ImGui::TextUnformatted(text.c_str());
            break;
        }

        case ProcessColumn::CpuTime:
        {
            const std::string text = UI::Format::formatCpuTimeCompact(proc.cpuTimeSeconds);
            ImGui::TextUnformatted(text.c_str());
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

void ProcessesPanel::renderProcessTreeNode(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                           const std::unordered_map<std::int32_t, std::vector<std::size_t>>& tree,
                                           const std::unordered_set<std::size_t>& filteredSet,
                                           std::size_t procIdx,
                                           int depth)
{
    const auto& proc = snapshots[procIdx];

    // Check if this process has children (in the filtered set)
    auto childrenIt = tree.find(proc.pid);
    bool hasChildren = false;
    std::vector<std::size_t> filteredChildren;

    if (childrenIt != tree.end())
    {
        // Only count children that are in the filtered set
        for (std::size_t childIdx : childrenIt->second)
        {
            if (filteredSet.contains(childIdx))
            {
                filteredChildren.push_back(childIdx);
            }
        }
        hasChildren = !filteredChildren.empty();
    }

    const bool isExpanded = !m_CollapsedPids.contains(proc.pid);

    // Render this process
    renderProcessRow(proc, depth, hasChildren, isExpanded);

    // Recursively render children if expanded
    if (hasChildren && isExpanded)
    {
        for (std::size_t childIdx : filteredChildren)
        {
            renderProcessTreeNode(snapshots, tree, filteredSet, childIdx, depth + 1);
        }
    }
}

void ProcessesPanel::renderTreeView(const std::vector<Domain::ProcessSnapshot>& snapshots,
                                    const std::vector<std::size_t>& filteredIndices,
                                    const std::unordered_map<std::int32_t, std::vector<std::size_t>>& tree)
{
    // Convert filtered indices to a set for O(1) lookups
    std::unordered_set<std::size_t> filteredSet(filteredIndices.begin(), filteredIndices.end());

    // Build a PID-to-index map for O(1) parent lookups within the filtered set
    std::unordered_map<std::int32_t, std::size_t> pidToIndex;
    for (std::size_t idx : filteredIndices)
    {
        pidToIndex[snapshots[idx].pid] = idx;
    }

    // Render root processes and their descendants
    for (std::size_t idx : filteredIndices)
    {
        const auto& proc = snapshots[idx];

        // Check if this process's parent is in the filtered set
        const bool parentInFilteredSet = pidToIndex.contains(proc.parentPid);

        // Only render if this is a root process (parent not in filtered set)
        if (!parentInFilteredSet)
        {
            renderProcessTreeNode(snapshots, tree, filteredSet, idx, 0);
        }
    }
}

} // namespace App
