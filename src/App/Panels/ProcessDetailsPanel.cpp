#include "ProcessDetailsPanel.h"

#include "Platform/Factory.h"
#include "UI/Format.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <string>

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
    std::string windowLabel;
    if (m_HasSnapshot && (m_SelectedPid != -1) && !m_CachedSnapshot.name.empty())
    {
        windowLabel = m_CachedSnapshot.name;
        windowLabel += "###ProcessDetails";
    }
    else
    {
        windowLabel = "Process Details###ProcessDetails";
    }

    if (!ImGui::Begin(windowLabel.c_str(), open))
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

    const char* titleCommand = !proc.command.empty() ? proc.command.c_str() : proc.name.c_str();
    ImGui::TextWrapped("Command Line: %s", titleCommand);
    ImGui::Spacing();

    // Compact layout: two label/value pairs per row.
    // Use fixed widths so columns don't jitter as values change (e.g., CPU time ticking).
    const ImGuiStyle& style = ImGui::GetStyle();
    const float labelWidth = ImGui::CalcTextSize("CPU Time").x + (style.CellPadding.x * 2.0F) + 10.0F;
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float spacingWidth = style.ItemSpacing.x * 3.0F;
    const float remaining = std::max(0.0F, availWidth - (labelWidth * 2.0F) - spacingWidth);
    const float valueWidth = remaining * 0.5F;

    if (ImGui::BeginTable("BasicInfo",
                          4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Label1", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("Value1", ImGuiTableColumnFlags_WidthFixed, valueWidth);
        ImGui::TableSetupColumn("Label2", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("Value2", ImGuiTableColumnFlags_WidthFixed, valueWidth);

        auto renderStatus = [&]()
        {
            const char stateChar = proc.displayState.empty() ? '?' : proc.displayState[0];
            ImVec4 statusColor;
            switch (stateChar)
            {
            case 'R': // Running
                statusColor = theme.scheme().statusRunning;
                break;
            case 'S': // Sleeping
                statusColor = theme.scheme().statusSleeping;
                break;
            case 'D': // Disk sleep
                statusColor = theme.scheme().statusDiskSleep;
                break;
            case 'Z': // Zombie
                statusColor = theme.scheme().statusZombie;
                break;
            case 'T': // Stopped/Traced
            case 't':
                statusColor = theme.scheme().statusStopped;
                break;
            case 'I': // Idle
                statusColor = theme.scheme().statusIdle;
                break;
            default:
                statusColor = theme.scheme().statusSleeping;
                break;
            }
            ImGui::TextColored(statusColor, "%s", proc.displayState.c_str());
        };

        auto renderCpuTime = [&]()
        {
            const int totalSeconds = static_cast<int>(proc.cpuTimeSeconds);
            const int hours = totalSeconds / 3600;
            const int minutes = (totalSeconds % 3600) / 60;
            const int seconds = totalSeconds % 60;
            const int centiseconds = static_cast<int>((proc.cpuTimeSeconds - static_cast<double>(totalSeconds)) * 100.0);

            if (hours > 0)
            {
                ImGui::Text("%d:%02d:%02d.%02d", hours, minutes, seconds, centiseconds);
            }
            else
            {
                ImGui::Text("%d:%02d.%02d", minutes, seconds, centiseconds);
            }
        };

        int fieldIndex = 0;
        auto addField = [&](const char* label, const auto& renderValue)
        {
            const int pairIndex = fieldIndex % 2;
            if (pairIndex == 0)
            {
                ImGui::TableNextRow();
            }

            ImGui::TableSetColumnIndex((pairIndex * 2));
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex((pairIndex * 2) + 1);
            renderValue();
            ++fieldIndex;
        };

        addField("PID", [&]() { ImGui::Text("%d", proc.pid); });
        addField("Parent", [&]() { ImGui::Text("%d", proc.parentPid); });

        addField("Name", [&]() { ImGui::TextUnformatted(proc.name.c_str()); });
        if (!proc.user.empty())
        {
            addField("User", [&]() { ImGui::TextUnformatted(proc.user.c_str()); });
        }

        addField("Status", renderStatus);

        if (proc.threadCount > 0)
        {
            addField("Threads", [&]() { ImGui::Text("%d", proc.threadCount); });
        }

        addField("Nice", [&]() { ImGui::Text("%d", proc.nice); });

        addField("CPU Time", renderCpuTime);

        ImGui::EndTable();
    }
}

void ProcessDetailsPanel::renderResourceUsage(const Domain::ProcessSnapshot& proc)
{
    const auto& theme = UI::Theme::get();

    ImGui::Text("Resource Usage");
    ImGui::Spacing();

    // CPU usage with progress bar
    ImGui::Text("CPU:");
    ImGui::SameLine(120.0F);
    float cpuFraction = static_cast<float>(proc.cpuPercent) / 100.0F;
    cpuFraction = (cpuFraction > 1.0F) ? 1.0F : cpuFraction; // Clamp for multi-core
    char cpuOverlay[32];
    snprintf(cpuOverlay, sizeof(cpuOverlay), "%s", UI::Format::percentCompact(proc.cpuPercent).c_str());
    ImGui::ProgressBar(cpuFraction, ImVec2(-1, 0), cpuOverlay);

    // Memory usage with progress bar
    ImGui::Text("Memory:");
    ImGui::SameLine(120.0F);
    float memFraction = static_cast<float>(proc.memoryPercent) / 100.0F;
    memFraction = (memFraction > 1.0F) ? 1.0F : memFraction;
    char memOverlay[32];
    snprintf(memOverlay, sizeof(memOverlay), "%s", UI::Format::percentCompact(proc.memoryPercent).c_str());
    ImGui::ProgressBar(memFraction, ImVec2(-1, 0), memOverlay);

    // Memory details with stacked horizontal bar
    double residentMB = static_cast<double>(proc.memoryBytes) / (1024.0 * 1024.0);
    double virtualMB = static_cast<double>(proc.virtualBytes) / (1024.0 * 1024.0);
    double sharedMB = static_cast<double>(proc.sharedBytes) / (1024.0 * 1024.0);

    ImGui::Spacing();
    ImGui::Text("Memory Breakdown:");

    // Stacked bar showing RSS vs Shared vs Virtual
    // Virtual is the max, RSS is physical, Shared is part of RSS that's shared
    if (virtualMB > 0)
    {
        float availWidth = ImGui::GetContentRegionAvail().x;
        float barHeight = 24.0F;

        ImVec2 barStart = ImGui::GetCursorScreenPos();
        const ImVec2 barSize(availWidth, barHeight);
        ImGui::InvisibleButton("##ProcessMemoryBreakdownBar", barSize);

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Calculate proportions (Virtual is the reference)
        float virtFrac = 1.0F;
        float rssFrac = static_cast<float>(residentMB / virtualMB);
        float shrFrac = static_cast<float>(sharedMB / virtualMB);

        // Clamp fractions
        rssFrac = std::min(rssFrac, 1.0F);
        shrFrac = std::min(shrFrac, rssFrac); // Shared can't exceed RSS

        // Colors for memory segments (use theme/ImGui primitives only)
        const ImU32 virtColor = ImGui::GetColorU32(ImGuiCol_FrameBgActive);
        const ImU32 rssColor = ImGui::ColorConvertFloat4ToU32(theme.scheme().chartMemory);
        const ImU32 shrColor = ImGui::ColorConvertFloat4ToU32(theme.scheme().chartIo);

        // Draw Virtual (full background)
        drawList->AddRectFilled(barStart, ImVec2(barStart.x + (availWidth * virtFrac), barStart.y + barHeight), virtColor);

        // Draw RSS on top
        drawList->AddRectFilled(barStart, ImVec2(barStart.x + (availWidth * rssFrac), barStart.y + barHeight), rssColor);

        // Draw Shared (subset of RSS)
        drawList->AddRectFilled(barStart, ImVec2(barStart.x + (availWidth * shrFrac), barStart.y + barHeight), shrColor);

        // Border
        drawList->AddRect(
            barStart, ImVec2(barStart.x + availWidth, barStart.y + barHeight), ImGui::ColorConvertFloat4ToU32(theme.scheme().border));

        // Helper to format memory size
        auto formatMem = [](double mb) -> std::string
        {
            if (mb >= 1024.0)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "%.1f GB", mb / 1024.0);
                return buf;
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f MB", mb);
            return buf;
        };

        // Overlay summary (centered)
        {
            char overlay[96];
            snprintf(overlay, sizeof(overlay), "RSS %s / VIRT %s", formatMem(residentMB).c_str(), formatMem(virtualMB).c_str());

            const ImVec2 textSize = ImGui::CalcTextSize(overlay);
            const ImVec2 textPos(barStart.x + ((availWidth - textSize.x) * 0.5F), barStart.y + ((barHeight - textSize.y) * 0.5F));
            const ImU32 shadowCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
            drawList->AddText(ImVec2(textPos.x + 1.0F, textPos.y + 1.0F), shadowCol, overlay);
            drawList->AddText(textPos, textCol, overlay);
        }

        // Detailed legend in tooltip on hover
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(rssColor), "Resident (RSS): %s", formatMem(residentMB).c_str());
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(shrColor), "Shared (SHR): %s", formatMem(sharedMB).c_str());
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(virtColor), "Virtual (VIRT): %s", formatMem(virtualMB).c_str());
            ImGui::EndTooltip();
        }
    }
    else
    {
        // Fallback to text if no virtual memory info
        ImGui::Text("Resident (RSS): %.1f MB", residentMB);
        ImGui::Text("Shared (SHR): %.1f MB", sharedMB);
        ImGui::Text("Virtual (VIRT): %.1f MB", virtualMB);
    }

    ImGui::Spacing();
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
    if (ImPlot::BeginPlot("##CPUHistory", ImVec2(-1, 200), ImPlotFlags_NoLegend))
    {
        ImPlot::SetupAxes("Time (s)", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

        if (cpuCount > 0)
        {
            ImVec4 fillColor = theme.scheme().chartCpu;
            fillColor.w = 0.3F;
            ImPlot::SetNextFillStyle(fillColor);
            ImPlot::PlotShaded("##CPUShaded", timeData.data(), cpuData.data(), static_cast<int>(cpuCount), 0.0);

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
    if (ImPlot::BeginPlot("##MemoryHistory", ImVec2(-1, 200), ImPlotFlags_NoLegend))
    {
        ImPlot::SetupAxes("Time (s)", "MB", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);

        if (memCount > 0)
        {
            ImVec4 fillColor = theme.scheme().chartMemory;
            fillColor.w = 0.3F;
            ImPlot::SetNextFillStyle(fillColor);
            ImPlot::PlotShaded("##MemoryShaded", timeData.data(), memData.data(), static_cast<int>(memCount), 0.0);

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
