#include "ProcessDetailsPanel.h"

#include "Platform/Factory.h"
#include "UI/Format.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>

namespace
{

[[nodiscard]] auto formatCpuTime(double seconds) -> std::string
{
    seconds = std::max(0.0, seconds);

    const uint64_t totalMs = static_cast<uint64_t>(seconds * 1000.0);
    const uint64_t hours = totalMs / (1000ULL * 60ULL * 60ULL);
    const uint64_t minutes = (totalMs / (1000ULL * 60ULL)) % 60ULL;
    const uint64_t secs = (totalMs / 1000ULL) % 60ULL;
    const uint64_t centis = (totalMs / 10ULL) % 100ULL;

    if (hours > 0)
    {
        return std::format("{}:{:02}:{:02}.{:02}", hours, minutes, secs, centis);
    }

    return std::format("{}:{:02}.{:02}", minutes, secs, centis);
}

} // namespace

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

    const auto computeLabelColumnWidth = []() -> float
    {
        // Keep labels from wrapping at large font sizes (prevents width/scrollbar jitter).
        constexpr std::array<const char*, 8> labels = {
            "PID",
            "Parent",
            "Name",
            "Status",
            "User",
            "Threads",
            "Nice",
            "CPU Time",
        };

        float maxTextWidth = 0.0F;
        for (const char* label : labels)
        {
            maxTextWidth = std::max(maxTextWidth, ImGui::CalcTextSize(label).x);
        }

        const ImGuiStyle& style = ImGui::GetStyle();
        return maxTextWidth + (style.CellPadding.x * 2.0F) + 6.0F;
    };

    const float labelColWidth = computeLabelColumnWidth();

    if (ImGui::BeginTable("BasicInfo",
                          4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                              ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label1", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, labelColWidth);
        ImGui::TableSetupColumn("Value1", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Label2", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, labelColWidth);
        ImGui::TableSetupColumn("Value2", ImGuiTableColumnFlags_WidthStretch);

        auto renderStatusValue = [&]()
        {
            ImVec4 statusColor = theme.scheme().textInfo;
            if (proc.displayState == "Running")
            {
                statusColor = theme.scheme().statusRunning;
            }
            else if (proc.displayState == "Sleeping")
            {
                statusColor = theme.scheme().statusSleeping;
            }
            else if (proc.displayState == "Disk Sleep")
            {
                statusColor = theme.scheme().statusDiskSleep;
            }
            else if (proc.displayState == "Zombie")
            {
                statusColor = theme.scheme().statusZombie;
            }
            else if (proc.displayState == "Stopped" || proc.displayState == "Tracing")
            {
                statusColor = theme.scheme().statusStopped;
            }
            else if (proc.displayState == "Idle")
            {
                statusColor = theme.scheme().statusIdle;
            }

            ImGui::TextColored(statusColor, "%s", proc.displayState.c_str());
        };

        auto addLabel = [](const char* text)
        {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(text);
        };

        auto addValueText = [](const char* text)
        {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(text);
        };

        // Row 1: PID / Parent PID
        ImGui::TableNextRow();
        addLabel("PID");
        ImGui::TableNextColumn();
        ImGui::Text("%d", proc.pid);
        addLabel("Parent");
        ImGui::TableNextColumn();
        ImGui::Text("%d", proc.parentPid);

        // Row 2: Name / Status
        ImGui::TableNextRow();
        addLabel("Name");
        addValueText(proc.name.c_str());
        addLabel("Status");
        ImGui::TableNextColumn();
        renderStatusValue();

        // Row 3: User / Threads
        ImGui::TableNextRow();
        addLabel("User");
        addValueText(proc.user.empty() ? "-" : proc.user.c_str());
        addLabel("Threads");
        ImGui::TableNextColumn();
        if (proc.threadCount > 0)
        {
            ImGui::Text("%d", proc.threadCount);
        }
        else
        {
            ImGui::TextUnformatted("-");
        }

        // Row 4: Nice / CPU Time
        ImGui::TableNextRow();
        addLabel("Nice");
        ImGui::TableNextColumn();
        ImGui::Text("%d", proc.nice);
        addLabel("CPU Time");
        addValueText(formatCpuTime(proc.cpuTimeSeconds).c_str());

        ImGui::EndTable();
    }
}

void ProcessDetailsPanel::renderResourceUsage(const Domain::ProcessSnapshot& proc)
{
    const auto& theme = UI::Theme::get();

    ImGui::Text("Resource Usage");
    ImGui::Spacing();

    // CPU usage with progress bar
    const float valueStartX = std::max(120.0F, ImGui::CalcTextSize("Memory:").x + 40.0F);
    ImGui::Text("CPU:");
    ImGui::SameLine(valueStartX);
    float cpuFraction = static_cast<float>(proc.cpuPercent) / 100.0F;
    cpuFraction = (cpuFraction > 1.0F) ? 1.0F : cpuFraction; // Clamp for multi-core
    const std::string cpuOverlay = UI::Format::percentCompact(proc.cpuPercent);
    const ImVec4 cpuColor = theme.progressColor(proc.cpuPercent);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, cpuColor);
    ImGui::ProgressBar(cpuFraction, ImVec2(-1, 0), "");
    UI::Widgets::drawRightAlignedOverlayText(cpuOverlay.c_str());
    ImGui::PopStyleColor();

    // Memory/VIRT stacked bar (Resident | Virtual remainder). Keep style consistent with other stacked bars.
    auto drawMemVirtStackedBar = [&](uint64_t residentBytes, uint64_t virtualBytes, const char* overlayText)
    {
        const uint64_t totalBytes = std::max(virtualBytes, residentBytes);
        if (totalBytes == 0)
        {
            return;
        }

        const uint64_t clampedResidentBytes = std::min(residentBytes, totalBytes);
        const uint64_t otherVirtualBytes = totalBytes - clampedResidentBytes;

        const float residentFrac = static_cast<float>(static_cast<double>(clampedResidentBytes) / static_cast<double>(totalBytes));
        const float otherFrac = static_cast<float>(static_cast<double>(otherVirtualBytes) / static_cast<double>(totalBytes));

        const ImVec2 startPos = ImGui::GetCursorScreenPos();
        const float fullWidth = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetFrameHeight();
        const ImVec2 size(fullWidth, height);
        ImGui::InvisibleButton("##MemVirtStackedBar", size);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float rounding = ImGui::GetStyle().FrameRounding;

        const ImU32 bgCol = ImGui::GetColorU32(ImGuiCol_FrameBg);
        const ImU32 residentCol = ImGui::ColorConvertFloat4ToU32(theme.scheme().chartMemory);
        const ImU32 otherCol = ImGui::ColorConvertFloat4ToU32(theme.scheme().chartIo);

        const ImVec2 endPos(startPos.x + size.x, startPos.y + size.y);
        drawList->AddRectFilled(startPos, endPos, bgCol, rounding);

        float x = startPos.x;
        const float residentW = size.x * residentFrac;
        const float otherW = size.x * otherFrac;

        if (residentW > 0.0F)
        {
            const ImDrawFlags flags = (otherW <= 0.0F) ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft;
            drawList->AddRectFilled(ImVec2(x, startPos.y), ImVec2(x + residentW, endPos.y), residentCol, rounding, flags);
            x += residentW;
        }

        if (otherW > 0.0F)
        {
            drawList->AddRectFilled(ImVec2(x, startPos.y), endPos, otherCol, rounding, ImDrawFlags_RoundCornersRight);
        }

        // Overlay text (consistent with other bars)
        UI::Widgets::drawRightAlignedOverlayText(overlayText);

        // Restore cursor position to below the bar.
        ImGui::SetCursorScreenPos(ImVec2(startPos.x, endPos.y + ImGui::GetStyle().ItemInnerSpacing.y));
    };

    const uint64_t totalVirtualBytes = std::max(proc.virtualBytes, proc.memoryBytes);
    const std::string overlay = UI::Format::bytesUsedTotalCompact(std::min(proc.memoryBytes, totalVirtualBytes), totalVirtualBytes);
    ImGui::TextUnformatted("Memory:");
    ImGui::SameLine(valueStartX);
    drawMemVirtStackedBar(proc.memoryBytes, proc.virtualBytes, overlay.c_str());

    if (ImGui::IsItemHovered() && totalVirtualBytes > 0)
    {
        const UI::Format::ByteUnit unit = UI::Format::unitForTotalBytes(totalVirtualBytes);
        ImGui::BeginTooltip();
        ImGui::Text("RSS: %s (%s)",
                    UI::Format::formatBytesWithUnit(proc.memoryBytes, unit).c_str(),
                    UI::Format::percentCompact(proc.memoryPercent).c_str());
        ImGui::Text("VIRT: %s", UI::Format::formatBytesWithUnit(totalVirtualBytes, unit).c_str());
        ImGui::EndTooltip();
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

    auto showHistoryTooltip =
        [](const char* title, const float* time, const float* values, size_t count, const std::function<std::string(float)>& formatValue)
    {
        if (count == 0)
        {
            return;
        }

        if (!ImPlot::IsPlotHovered())
        {
            return;
        }

        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
        int index = static_cast<int>(std::llround(mouse.x + static_cast<double>(count - 1)));
        index = std::clamp(index, 0, static_cast<int>(count) - 1);

        if (ImGui::BeginTooltip())
        {
            ImGui::TextUnformatted(title);
            ImGui::Separator();
            const int timeSec = static_cast<int>(std::lround(static_cast<double>(time[index])));
            ImGui::Text("t: %ds", timeSec);
            const std::string formatted = formatValue(values[index]);
            ImGui::TextUnformatted(formatted.c_str());
            ImGui::EndTooltip();
        }
    };

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

        showHistoryTooltip("CPU Usage",
                           timeData.data(),
                           cpuData.data(),
                           cpuCount,
                           [](float value) { return std::format("CPU: {}", UI::Format::percentCompact(static_cast<double>(value))); });

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

        showHistoryTooltip("Memory Usage (RSS)",
                           timeData.data(),
                           memData.data(),
                           memCount,
                           [](float value) { return std::format("RSS: {:.1f} MB", value); });

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
