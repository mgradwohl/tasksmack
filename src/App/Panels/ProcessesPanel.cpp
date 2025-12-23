#include "ProcessesPanel.h"
#include "imgui.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

ProcessesPanel::ProcessesPanel()
    : m_sortColumn(ProcessColumn::CPU)
    , m_sortAscending(false)
    , m_filterText("")
    , m_showTreeView(false)
{
}

void ProcessesPanel::Render(const std::vector<ProcessInfo>& processes)
{
    if (!ImGui::BeginChild("ProcessesPanel", ImVec2(0, 0), true))
    {
        ImGui::EndChild();
        return;
    }

    // Filter input
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##filter", m_filterText, sizeof(m_filterText));
    ImGui::SameLine();
    ImGui::Text("Filter");

    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        m_filterText[0] = '\0';
    }

    // Apply filter
    std::vector<ProcessInfo> filteredProcesses;
    for (const auto& proc : processes)
    {
        if (m_filterText[0] == '\0' ||
            proc.name.find(m_filterText) != std::string::npos ||
            std::to_string(proc.pid).find(m_filterText) != std::string::npos)
        {
            filteredProcesses.push_back(proc);
        }
    }

    // Sort
    std::sort(filteredProcesses.begin(), filteredProcesses.end(),
        [this](const ProcessInfo& a, const ProcessInfo& b) {
            bool less = false;
            switch (m_sortColumn)
            {
            case ProcessColumn::PID:
                less = a.pid < b.pid;
                break;
            case ProcessColumn::Name:
                less = a.name < b.name;
                break;
            case ProcessColumn::CPU:
                less = a.cpuPercent < b.cpuPercent;
                break;
            case ProcessColumn::Memory:
                less = a.memoryMB < b.memoryMB;
                break;
            case ProcessColumn::Threads:
                less = a.threadCount < b.threadCount;
                break;
            }
            return m_sortAscending ? less : !less;
        });

    // Build tree structure if in tree view mode
    std::map<uint32_t, std::vector<const ProcessInfo*>> childMap;
    if (m_showTreeView)
    {
        for (const auto& proc : filteredProcesses)
        {
            childMap[proc.parentPid].push_back(&proc);
        }
    }

    // Process table
    ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("ProcessTable", 5, flags))
    {
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 80.0f, (int)ProcessColumn::PID);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.0f, (int)ProcessColumn::Name);
        ImGui::TableSetupColumn("CPU %", ImGuiTableColumnFlags_WidthFixed, 80.0f, (int)ProcessColumn::CPU);
        ImGui::TableSetupColumn("Memory (MB)", ImGuiTableColumnFlags_WidthFixed, 100.0f, (int)ProcessColumn::Memory);
        ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_WidthFixed, 80.0f, (int)ProcessColumn::Threads);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Handle sorting
        ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
        if (sortSpecs && sortSpecs->SpecsDirty)
        {
            if (sortSpecs->SpecsCount > 0)
            {
                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                m_sortColumn = (ProcessColumn)spec.ColumnUserID;
                m_sortAscending = spec.SortDirection == ImGuiSortDirection_Ascending;
            }
            sortSpecs->SpecsDirty = false;
        }

        // Render processes
        if (m_showTreeView)
        {
            // Find root processes (those whose parent is not in filtered list)
            std::set<uint32_t> filteredPids;
            for (const auto& proc : filteredProcesses)
            {
                filteredPids.insert(proc.pid);
            }

            for (const auto& proc : filteredProcesses)
            {
                if (filteredPids.find(proc.parentPid) == filteredPids.end())
                {
                    RenderProcessTreeNode(proc, childMap, 0);
                }
            }
        }
        else
        {
            // Flat list view
            for (const auto& proc : filteredProcesses)
            {
                RenderProcessRow(proc, 0);
            }
        }

        ImGui::EndTable();
    }

    // Summary
    ImGui::Separator();
    
    // Tree view toggle button
    if (ImGui::Button(m_showTreeView ? "List View" : "Tree View"))
    {
        m_showTreeView = !m_showTreeView;
    }
    ImGui::SameLine();
    
    // Calculate and render summary text
    float totalCPU = 0.0f;
    float totalMemory = 0.0f;
    for (const auto& proc : filteredProcesses)
    {
        totalCPU += proc.cpuPercent;
        totalMemory += proc.memoryMB;
    }

    std::stringstream ss;
    ss << "Processes: " << filteredProcesses.size()
        << " | Total CPU: " << std::fixed << std::setprecision(1) << totalCPU << "%"
        << " | Total Memory: " << std::fixed << std::setprecision(1) << totalMemory << " MB";

    ImGui::Text("%s", ss.str().c_str());

    ImGui::EndChild();
}

void ProcessesPanel::RenderProcessRow(const ProcessInfo& proc, int indentLevel)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    // PID
    ImGui::Text("%u", proc.pid);

    ImGui::TableNextColumn();
    // Name with indent
    if (indentLevel > 0)
    {
        ImGui::Indent(indentLevel * 20.0f);
    }
    ImGui::Text("%s", proc.name.c_str());
    if (indentLevel > 0)
    {
        ImGui::Unindent(indentLevel * 20.0f);
    }

    ImGui::TableNextColumn();
    // CPU
    ImGui::Text("%.1f", proc.cpuPercent);

    ImGui::TableNextColumn();
    // Memory
    ImGui::Text("%.1f", proc.memoryMB);

    ImGui::TableNextColumn();
    // Threads
    ImGui::Text("%u", proc.threadCount);
}

void ProcessesPanel::RenderProcessTreeNode(const ProcessInfo& proc,
    const std::map<uint32_t, std::vector<const ProcessInfo*>>& childMap,
    int indentLevel)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    // Check if this process has children
    auto it = childMap.find(proc.pid);
    bool hasChildren = (it != childMap.end() && !it->second.empty());

    // PID with tree node
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth;
    if (!hasChildren)
    {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    bool nodeOpen = false;
    if (indentLevel > 0)
    {
        ImGui::Indent(indentLevel * 20.0f);
    }

    if (hasChildren)
    {
        nodeOpen = ImGui::TreeNodeEx((void*)(intptr_t)proc.pid, flags, "%u", proc.pid);
    }
    else
    {
        ImGui::TreeNodeEx((void*)(intptr_t)proc.pid, flags, "%u", proc.pid);
    }

    if (indentLevel > 0)
    {
        ImGui::Unindent(indentLevel * 20.0f);
    }

    ImGui::TableNextColumn();
    // Name
    ImGui::Text("%s", proc.name.c_str());

    ImGui::TableNextColumn();
    // CPU
    ImGui::Text("%.1f", proc.cpuPercent);

    ImGui::TableNextColumn();
    // Memory
    ImGui::Text("%.1f", proc.memoryMB);

    ImGui::TableNextColumn();
    // Threads
    ImGui::Text("%u", proc.threadCount);

    // Render children
    if (hasChildren && nodeOpen)
    {
        for (const auto* child : it->second)
        {
            RenderProcessTreeNode(*child, childMap, indentLevel + 1);
        }
        ImGui::TreePop();
    }
}
