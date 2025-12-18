#include "ProcessDetailsPanel.h"

#include "Platform/Factory.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <array>

namespace App
{

ProcessDetailsPanel::ProcessDetailsPanel()
    : Panel("Process Details"),
      m_ProcessActions(Platform::makeProcessActions()),
      m_ActionCapabilities(m_ProcessActions->actionCapabilities())
{
}

void ProcessDetailsPanel::updateWithSnapshot(const Domain::ProcessSnapshot* snapshot, float deltaTime)
{
    // Fade out action result message
    if (m_ActionResultTimer > 0.0F)
    {
        m_ActionResultTimer -= deltaTime;
        if (m_ActionResultTimer <= 0.0F)
        {
            m_LastActionResult.clear();
        }
    }

    if (snapshot != nullptr && snapshot->pid == m_SelectedPid)
    {
        m_CachedSnapshot = *snapshot;
        m_HasSnapshot = true;

        // Sample history at fixed interval
        m_HistoryTimer += deltaTime;
        if (m_HistoryTimer >= HISTORY_SAMPLE_INTERVAL)
        {
            m_HistoryTimer = 0.0F;
            m_CpuHistory.push(static_cast<float>(snapshot->cpuPercent));
            m_MemoryHistory.push(static_cast<float>(static_cast<double>(snapshot->memoryBytes) / (1024.0 * 1024.0)));
        }
    }
    else if (snapshot == nullptr || snapshot->pid != m_SelectedPid)
    {
        // Selection changed or no selection
        if (m_SelectedPid == -1)
        {
            m_HasSnapshot = false;
        }
    }
}

void ProcessDetailsPanel::render(bool* open)
{
    if (!ImGui::Begin("Process Details", open))
    {
        ImGui::End();
        return;
    }

    if (m_SelectedPid == -1)
    {
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textMuted, "Select a process from the Processes panel to view details");
        ImGui::End();
        return;
    }

    if (!m_HasSnapshot)
    {
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textWarning, "Process %d not found (may have exited)", m_SelectedPid);
        ImGui::End();
        return;
    }

    // Tabs for different info sections
    if (ImGui::BeginTabBar("DetailsTabs"))
    {
        if (ImGui::BeginTabItem("Overview"))
        {
            renderBasicInfo(m_CachedSnapshot);
            ImGui::Separator();
            renderResourceUsage(m_CachedSnapshot);
            ImGui::Separator();
            renderIoStats(m_CachedSnapshot);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("History"))
        {
            renderHistoryGraphs();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Actions"))
        {
            renderActions();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

void ProcessDetailsPanel::setSelectedPid(int32_t pid)
{
    if (pid != m_SelectedPid)
    {
        m_SelectedPid = pid;
        m_CpuHistory.clear();
        m_MemoryHistory.clear();
        m_HistoryTimer = 0.0F;
        m_HasSnapshot = false;
        m_ShowConfirmDialog = false;
        m_LastActionResult.clear();

        if (pid != -1)
        {
            spdlog::debug("ProcessDetailsPanel: selected PID {}", pid);
        }
    }
}

void ProcessDetailsPanel::renderBasicInfo(const Domain::ProcessSnapshot& proc)
{
    const auto& theme = UI::Theme::get();

    ImGui::Text("Process Information");
    ImGui::Spacing();

    if (ImGui::BeginTable("BasicInfo", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0F);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        // PID
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(theme.scheme().textMuted, "PID");
        ImGui::TableNextColumn();
        ImGui::Text("%d", proc.pid);

        // Parent PID
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(theme.scheme().textMuted, "Parent PID");
        ImGui::TableNextColumn();
        ImGui::Text("%d", proc.parentPid);

        // Name
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(theme.scheme().textMuted, "Name");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(proc.name.c_str());

        // Status (color-coded)
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(theme.scheme().textMuted, "Status");
        ImGui::TableNextColumn();
        ImVec4 statusColor = theme.scheme().textInfo;
        if (proc.displayState == "Running")
        {
            statusColor = theme.scheme().statusRunning;
        }
        else if (proc.displayState == "Zombie")
        {
            statusColor = theme.scheme().textError;
        }
        else if (proc.displayState == "Stopped")
        {
            statusColor = theme.scheme().statusStopped;
        }
        ImGui::TextColored(statusColor, "%s", proc.displayState.c_str());

        // Threads (if available)
        if (proc.threadCount > 0)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(theme.scheme().textMuted, "Threads");
            ImGui::TableNextColumn();
            ImGui::Text("%d", proc.threadCount);
        }

        // Nice/Priority
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(theme.scheme().textMuted, "Nice");
        ImGui::TableNextColumn();
        ImGui::Text("%d", proc.nice);

        ImGui::EndTable();
    }

    // Command line (separate section for long text with wrapping)
    if (!proc.command.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(theme.scheme().textMuted, "Command Line:");
        ImGui::Indent();
        ImGui::TextWrapped("%s", proc.command.c_str());
        ImGui::Unindent();
    }
}

void ProcessDetailsPanel::renderResourceUsage(const Domain::ProcessSnapshot& proc)
{
    ImGui::Text("Resource Usage");
    ImGui::Spacing();

    // CPU usage with progress bar
    ImGui::Text("CPU:");
    ImGui::SameLine(120.0F);
    float cpuFraction = static_cast<float>(proc.cpuPercent) / 100.0F;
    cpuFraction = (cpuFraction > 1.0F) ? 1.0F : cpuFraction; // Clamp for multi-core
    char cpuOverlay[32];
    snprintf(cpuOverlay, sizeof(cpuOverlay), "%.1f%%", proc.cpuPercent);
    ImGui::ProgressBar(cpuFraction, ImVec2(-1, 0), cpuOverlay);

    // Memory usage
    double memoryMB = static_cast<double>(proc.memoryBytes) / (1024.0 * 1024.0);
    double virtualMB = static_cast<double>(proc.virtualBytes) / (1024.0 * 1024.0);

    ImGui::Text("Memory (RSS):");
    ImGui::SameLine(120.0F);
    if (memoryMB >= 1024.0)
    {
        ImGui::Text("%.2f GB", memoryMB / 1024.0);
    }
    else
    {
        ImGui::Text("%.1f MB", memoryMB);
    }

    ImGui::Text("Virtual Memory:");
    ImGui::SameLine(120.0F);
    if (virtualMB >= 1024.0)
    {
        ImGui::Text("%.2f GB", virtualMB / 1024.0);
    }
    else
    {
        ImGui::Text("%.1f MB", virtualMB);
    }

    ImGui::Spacing();

    // CPU Time (cumulative)
    auto totalSeconds = static_cast<int>(proc.cpuTimeSeconds);
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int seconds = totalSeconds % 60;
    int centiseconds = static_cast<int>((proc.cpuTimeSeconds - static_cast<double>(totalSeconds)) * 100.0);

    ImGui::Text("CPU Time:");
    ImGui::SameLine(120.0F);
    if (hours > 0)
    {
        ImGui::Text("%d:%02d:%02d.%02d", hours, minutes, seconds, centiseconds);
    }
    else
    {
        ImGui::Text("%d:%02d.%02d", minutes, seconds, centiseconds);
    }
}

void ProcessDetailsPanel::renderIoStats(const Domain::ProcessSnapshot& proc)
{
    // Only show I/O stats if we have data
    if (proc.ioReadBytesPerSec == 0.0 && proc.ioWriteBytesPerSec == 0.0)
    {
        return;
    }

    ImGui::Text("I/O Statistics");
    ImGui::Spacing();

    auto formatRate = [](double bytesPerSec) -> std::pair<double, const char*>
    {
        if (bytesPerSec >= 1024.0 * 1024.0)
        {
            return {bytesPerSec / (1024.0 * 1024.0), "MB/s"};
        }
        if (bytesPerSec >= 1024.0)
        {
            return {bytesPerSec / 1024.0, "KB/s"};
        }
        return {bytesPerSec, "B/s"};
    };

    auto [readVal, readUnit] = formatRate(proc.ioReadBytesPerSec);
    auto [writeVal, writeUnit] = formatRate(proc.ioWriteBytesPerSec);

    ImGui::Text("Read:");
    ImGui::SameLine(80.0F);
    ImGui::Text("%.1f %s", readVal, readUnit);

    ImGui::Text("Write:");
    ImGui::SameLine(80.0F);
    ImGui::Text("%.1f %s", writeVal, writeUnit);
}

void ProcessDetailsPanel::renderHistoryGraphs()
{
    ImGui::Text("Resource History (last %zu samples)", m_CpuHistory.size());
    ImGui::Spacing();

    // Prepare data arrays for plotting
    std::array<float, HISTORY_SIZE> cpuData{};
    std::array<float, HISTORY_SIZE> memData{};
    std::array<float, HISTORY_SIZE> timeData{};

    size_t cpuCount = m_CpuHistory.copyTo(cpuData.data(), HISTORY_SIZE);
    size_t memCount = m_MemoryHistory.copyTo(memData.data(), HISTORY_SIZE);

    // Generate time axis (negative = past)
    for (size_t i = 0; i < cpuCount; ++i)
    {
        timeData[i] = static_cast<float>(i) - static_cast<float>(cpuCount - 1);
    }

    // CPU History Plot
    const auto& theme = UI::Theme::get();
    ImGui::Text("CPU Usage");
    if (ImPlot::BeginPlot("##CPUHistory", ImVec2(-1, 150), ImPlotFlags_NoLegend))
    {
        ImPlot::SetupAxes("Time (s)", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

        if (cpuCount > 0)
        {
            ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
            ImPlot::PlotLine("CPU", timeData.data(), cpuData.data(), static_cast<int>(cpuCount));
        }
        else
        {
            ImPlot::PlotDummy("CPU");
        }

        ImPlot::EndPlot();
    }

    ImGui::Spacing();

    // Memory History Plot
    ImGui::Text("Memory Usage (RSS)");
    if (ImPlot::BeginPlot("##MemoryHistory", ImVec2(-1, 150), ImPlotFlags_NoLegend))
    {
        ImPlot::SetupAxes("Time (s)", "MB", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);

        if (memCount > 0)
        {
            ImPlot::SetNextLineStyle(theme.scheme().chartMemory, 2.0F);
            ImPlot::PlotLine("Memory", timeData.data(), memData.data(), static_cast<int>(memCount));
        }
        else
        {
            ImPlot::PlotDummy("Memory");
        }

        ImPlot::EndPlot();
    }
}

void ProcessDetailsPanel::renderActions()
{
    const auto& theme = UI::Theme::get();

    ImGui::Text("Process Actions");
    ImGui::Spacing();
    ImGui::TextColored(theme.scheme().textMuted, "Target: %s (PID %d)", m_CachedSnapshot.name.c_str(), m_SelectedPid);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Warning text
    ImGui::TextColored(theme.scheme().textWarning, "Warning: These actions affect the running process!");
    ImGui::Spacing();

    // Action result feedback
    if (!m_LastActionResult.empty())
    {
        bool isError = m_LastActionResult.contains("Error") || m_LastActionResult.contains("Failed");
        ImVec4 color = isError ? theme.scheme().textError : theme.scheme().textSuccess;
        ImGui::TextColored(color, "%s", m_LastActionResult.c_str());
        ImGui::Spacing();
    }

    // Confirmation dialog
    if (m_ShowConfirmDialog)
    {
        ImGui::OpenPopup("Confirm Action");
    }

    if (ImGui::BeginPopupModal("Confirm Action", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text(
            "Are you sure you want to %s process '%s' (PID %d)?", m_ConfirmAction.c_str(), m_CachedSnapshot.name.c_str(), m_SelectedPid);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Yes", ImVec2(120, 0)))
        {
            Platform::ProcessActionResult result;

            if (m_ConfirmAction == "terminate")
            {
                result = m_ProcessActions->terminate(m_SelectedPid);
            }
            else if (m_ConfirmAction == "kill")
            {
                result = m_ProcessActions->kill(m_SelectedPid);
            }
            else if (m_ConfirmAction == "stop")
            {
                result = m_ProcessActions->stop(m_SelectedPid);
            }
            else if (m_ConfirmAction == "resume")
            {
                result = m_ProcessActions->resume(m_SelectedPid);
            }

            if (result.success)
            {
                m_LastActionResult = "Success: " + m_ConfirmAction + " sent to PID " + std::to_string(m_SelectedPid);
            }
            else
            {
                m_LastActionResult = "Error: " + result.errorMessage;
            }
            m_ActionResultTimer = 5.0F;

            m_ShowConfirmDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("No", ImVec2(120, 0)))
        {
            m_ShowConfirmDialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // Action buttons
    ImGui::BeginGroup();

    // Terminate (SIGTERM) - graceful
    if (m_ActionCapabilities.canTerminate)
    {
        if (ImGui::Button("Terminate (SIGTERM)", ImVec2(180, 0)))
        {
            m_ConfirmAction = "terminate";
            m_ShowConfirmDialog = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Send SIGTERM - request graceful shutdown");
        }
    }

    ImGui::SameLine();

    // Kill (SIGKILL) - forceful
    if (m_ActionCapabilities.canKill)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, theme.scheme().dangerButton);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.scheme().dangerButtonHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.scheme().dangerButtonActive);
        if (ImGui::Button("Kill (SIGKILL)", ImVec2(180, 0)))
        {
            m_ConfirmAction = "kill";
            m_ShowConfirmDialog = true;
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Send SIGKILL - force kill (cannot be caught)");
        }
    }

    ImGui::Spacing();

    // Stop (SIGSTOP) - pause
    if (m_ActionCapabilities.canStop)
    {
        if (ImGui::Button("Stop (SIGSTOP)", ImVec2(180, 0)))
        {
            m_ConfirmAction = "stop";
            m_ShowConfirmDialog = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Send SIGSTOP - pause the process");
        }
    }

    ImGui::SameLine();

    // Continue (SIGCONT) - resume
    if (m_ActionCapabilities.canContinue)
    {
        if (ImGui::Button("Resume (SIGCONT)", ImVec2(180, 0)))
        {
            m_ConfirmAction = "resume";
            m_ShowConfirmDialog = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Send SIGCONT - resume a stopped process");
        }
    }

    ImGui::EndGroup();
}

} // namespace App
