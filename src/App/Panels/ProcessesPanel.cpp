#include "ProcessesPanel.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"
#include <algorithm>
#include <psapi.h>
#include <TlHelp32.h>

ProcessesPanel::ProcessesPanel()
    : m_sortColumn(ProcessColumn::CPU)
    , m_sortAscending(false)
    , m_selectedPID(0)
    , m_updateCounter(0)
    , m_showTreeView(false)
{
}

void ProcessesPanel::Update()
{
    m_updateCounter++;
    if (m_updateCounter % 30 == 0) // Update every 30 frames (~0.5 seconds at 60 FPS)
    {
        RefreshProcessList();
    }
}

void ProcessesPanel::Render()
{
    if (ImGui::Begin("Processes"))
    {
        RenderToolbar();
        RenderProcessTable();
    }
    ImGui::End();
}

void ProcessesPanel::RenderToolbar()
{
    // Search box
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputTextWithHint("##search", "Search processes...", &m_searchText))
    {
        // Filter updates automatically through m_searchText
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
    {
        RefreshProcessList();
    }

    ImGui::SameLine();
    if (ImGui::Button("End Task"))
    {
        if (m_selectedPID != 0)
        {
            TerminateProcess(m_selectedPID);
        }
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::Text("Select a process and click 'End Task' to terminate it");
        ImGui::EndTooltip();
    }
}

void ProcessesPanel::RenderProcessTable()
{
    // Summary
    ImGui::Separator();
    ImGui::Text("Processes: %zu", m_processes.size());
    
    ImGui::SameLine();
    float totalCPU = 0.0f;
    for (const auto& proc : m_processes)
    {
        totalCPU += proc.cpuUsage;
    }
    ImGui::Text("| Total CPU: %.1f%%", totalCPU);

    ImGui::SameLine();
    SIZE_T totalMemory = 0;
    for (const auto& proc : m_processes)
    {
        totalMemory += proc.memoryUsage;
    }
    ImGui::Text("| Total Memory: %.1f MB", totalMemory / (1024.0f * 1024.0f));

    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        m_searchText.clear();
        m_selectedPID = 0;
    }

    ImGui::SameLine();
    if (ImGui::Checkbox("Tree View", &m_showTreeView))
    {
        if (m_showTreeView)
        {
            BuildProcessTree();
        }
    }

    ImGui::Separator();

    // Filter processes based on search
    std::vector<ProcessInfo> filteredProcesses;
    for (const auto& proc : m_processes)
    {
        if (m_searchText.empty() ||
            proc.name.find(m_searchText) != std::string::npos ||
            std::to_string(proc.pid).find(m_searchText) != std::string::npos)
        {
            filteredProcesses.push_back(proc);
        }
    }

    // Table
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_Reorderable;

    if (m_showTreeView)
    {
        if (ImGui::BeginTable("ProcessesTable", 5, flags))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.0f, (int)ProcessColumn::Name);
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80.0f, (int)ProcessColumn::PID);
            ImGui::TableSetupColumn("CPU %", ImGuiTableColumnFlags_WidthFixed, 80.0f, (int)ProcessColumn::CPU);
            ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, 100.0f, (int)ProcessColumn::Memory);
            ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_WidthFixed, 80.0f, (int)ProcessColumn::Threads);
            ImGui::TableHeadersRow();

            // Handle sorting
            if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
            {
                if (sortSpecs->SpecsDirty)
                {
                    if (sortSpecs->SpecsCount > 0)
                    {
                        m_sortColumn = (ProcessColumn)sortSpecs->Specs[0].ColumnUserID;
                        m_sortAscending = sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                        SortProcesses(filteredProcesses);
                    }
                    sortSpecs->SpecsDirty = false;
                }
            }

            // Render tree view
            RenderProcessTree(filteredProcesses);

            ImGui::EndTable();
        }
    }
    else
    {
        if (ImGui::BeginTable("ProcessesTable", 5, flags))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.0f, (int)ProcessColumn::Name);
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 80.0f, (int)ProcessColumn::PID);
            ImGui::TableSetupColumn("CPU %", ImGuiTableColumnFlags_WidthFixed, 80.0f, (int)ProcessColumn::CPU);
            ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, 100.0f, (int)ProcessColumn::Memory);
            ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_WidthFixed, 80.0f, (int)ProcessColumn::Threads);
            ImGui::TableHeadersRow();

            // Handle sorting
            if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs())
            {
                if (sortSpecs->SpecsDirty)
                {
                    if (sortSpecs->SpecsCount > 0)
                    {
                        m_sortColumn = (ProcessColumn)sortSpecs->Specs[0].ColumnUserID;
                        m_sortAscending = sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                        SortProcesses(filteredProcesses);
                    }
                    sortSpecs->SpecsDirty = false;
                }
            }

            // Render rows
            for (const auto& proc : filteredProcesses)
            {
                ImGui::TableNextRow();
                RenderProcessRow(proc);
            }

            ImGui::EndTable();
        }
    }
}

void ProcessesPanel::RenderProcessRow(const ProcessInfo& proc)
{
    ImGui::TableNextColumn();
    
    ImGuiSelectableFlags selectableFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap;
    bool isSelected = (m_selectedPID == proc.pid);
    
    if (ImGui::Selectable(proc.name.c_str(), isSelected, selectableFlags))
    {
        m_selectedPID = proc.pid;
    }

    ImGui::TableNextColumn();
    ImGui::Text("%lu", proc.pid);

    ImGui::TableNextColumn();
    ImGui::Text("%.1f", proc.cpuUsage);

    ImGui::TableNextColumn();
    ImGui::Text("%.1f MB", proc.memoryUsage / (1024.0f * 1024.0f));

    ImGui::TableNextColumn();
    ImGui::Text("%lu", proc.threadCount);
}

void ProcessesPanel::RefreshProcessList()
{
    m_processes.clear();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(snapshot, &pe32))
    {
        do
        {
            ProcessInfo info;
            info.pid = pe32.th32ProcessID;
            info.name = pe32.szExeFile;
            info.threadCount = pe32.cntThreads;
            info.parentPID = pe32.th32ParentProcessID;

            // Get memory and CPU info
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe32.th32ProcessID);
            if (hProcess)
            {
                PROCESS_MEMORY_COUNTERS pmc;
                if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc)))
                {
                    info.memoryUsage = pmc.WorkingSetSize;
                }

                // CPU usage calculation would require tracking over time
                // For now, we'll set it to 0
                info.cpuUsage = 0.0f;

                CloseHandle(hProcess);
            }

            m_processes.push_back(info);

        } while (Process32Next(snapshot, &pe32));
    }

    CloseHandle(snapshot);

    // Sort by current column
    SortProcesses(m_processes);

    if (m_showTreeView)
    {
        BuildProcessTree();
    }
}

void ProcessesPanel::SortProcesses(std::vector<ProcessInfo>& processes)
{
    std::sort(processes.begin(), processes.end(), [this](const ProcessInfo& a, const ProcessInfo& b)
    {
        bool result = false;
        switch (m_sortColumn)
        {
            case ProcessColumn::Name:
                result = a.name < b.name;
                break;
            case ProcessColumn::PID:
                result = a.pid < b.pid;
                break;
            case ProcessColumn::CPU:
                result = a.cpuUsage < b.cpuUsage;
                break;
            case ProcessColumn::Memory:
                result = a.memoryUsage < b.memoryUsage;
                break;
            case ProcessColumn::Threads:
                result = a.threadCount < b.threadCount;
                break;
        }
        return m_sortAscending ? result : !result;
    });
}

void ProcessesPanel::TerminateProcess(DWORD pid)
{
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProcess)
    {
        ::TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        RefreshProcessList();
        m_selectedPID = 0;
    }
}

void ProcessesPanel::BuildProcessTree()
{
    m_processTree.clear();
    m_pidToNodeMap.clear();

    // Create nodes for all processes
    for (const auto& proc : m_processes)
    {
        auto node = std::make_shared<ProcessTreeNode>();
        node->process = proc;
        m_pidToNodeMap[proc.pid] = node;
    }

    // Build parent-child relationships
    for (const auto& proc : m_processes)
    {
        auto node = m_pidToNodeMap[proc.pid];
        
        // Find parent
        auto parentIt = m_pidToNodeMap.find(proc.parentPID);
        if (parentIt != m_pidToNodeMap.end() && proc.pid != proc.parentPID)
        {
            node->parent = parentIt->second;
            parentIt->second->children.push_back(node);
        }
        else
        {
            // Root level process (no parent or parent not found)
            m_processTree.push_back(node);
        }
    }

    // Sort children at each level
    std::function<void(std::shared_ptr<ProcessTreeNode>)> sortChildren;
    sortChildren = [&](std::shared_ptr<ProcessTreeNode> node)
    {
        SortProcesses(node->children, node);
        for (auto& child : node->children)
        {
            sortChildren(child);
        }
    };

    for (auto& root : m_processTree)
    {
        sortChildren(root);
    }
}

void ProcessesPanel::SortProcesses(std::vector<std::shared_ptr<ProcessTreeNode>>& nodes, std::shared_ptr<ProcessTreeNode> parent)
{
    std::sort(nodes.begin(), nodes.end(), [this](const std::shared_ptr<ProcessTreeNode>& a, const std::shared_ptr<ProcessTreeNode>& b)
    {
        bool result = false;
        switch (m_sortColumn)
        {
            case ProcessColumn::Name:
                result = a->process.name < b->process.name;
                break;
            case ProcessColumn::PID:
                result = a->process.pid < b->process.pid;
                break;
            case ProcessColumn::CPU:
                result = a->process.cpuUsage < b->process.cpuUsage;
                break;
            case ProcessColumn::Memory:
                result = a->process.memoryUsage < b->process.memoryUsage;
                break;
            case ProcessColumn::Threads:
                result = a->process.threadCount < b->process.threadCount;
                break;
        }
        return m_sortAscending ? result : !result;
    });
}

void ProcessesPanel::RenderProcessTree(const std::vector<ProcessInfo>& filteredProcesses)
{
    // Create a set of filtered PIDs for quick lookup
    std::unordered_set<DWORD> filteredPIDs;
    for (const auto& proc : filteredProcesses)
    {
        filteredPIDs.insert(proc.pid);
    }

    std::function<void(std::shared_ptr<ProcessTreeNode>, int)> renderNode;
    renderNode = [&](std::shared_ptr<ProcessTreeNode> node, int depth)
    {
        // Skip if not in filtered list
        if (filteredPIDs.find(node->process.pid) == filteredPIDs.end())
            return;

        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        // Indent based on depth
        for (int i = 0; i < depth; i++)
        {
            ImGui::Indent(10.0f);
        }

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
        
        // Check if any children are in the filtered list
        bool hasFilteredChildren = false;
        for (const auto& child : node->children)
        {
            if (filteredPIDs.find(child->process.pid) != filteredPIDs.end())
            {
                hasFilteredChildren = true;
                break;
            }
        }

        if (!hasFilteredChildren)
        {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }

        bool isSelected = (m_selectedPID == node->process.pid);
        if (isSelected)
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        bool nodeOpen = ImGui::TreeNodeEx((void*)(intptr_t)node->process.pid, flags, "%s", node->process.name.c_str());

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            m_selectedPID = node->process.pid;
        }

        // Render other columns
        ImGui::TableNextColumn();
        ImGui::Text("%lu", node->process.pid);

        ImGui::TableNextColumn();
        ImGui::Text("%.1f", node->process.cpuUsage);

        ImGui::TableNextColumn();
        ImGui::Text("%.1f MB", node->process.memoryUsage / (1024.0f * 1024.0f));

        ImGui::TableNextColumn();
        ImGui::Text("%lu", node->process.threadCount);

        // Render children
        if (nodeOpen && hasFilteredChildren)
        {
            for (auto& child : node->children)
            {
                renderNode(child, depth + 1);
            }
            ImGui::TreePop();
        }

        // Unindent
        for (int i = 0; i < depth; i++)
        {
            ImGui::Unindent(10.0f);
        }
    };

    // Render all root nodes
    for (auto& root : m_processTree)
    {
        renderNode(root, 0);
    }
}
