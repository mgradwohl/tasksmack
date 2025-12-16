#include "SystemMetricsPanel.h"

#include "Platform/Factory.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace App
{

SystemMetricsPanel::SystemMetricsPanel() : Panel("System")
{
}

SystemMetricsPanel::~SystemMetricsPanel()
{
    // Direct cleanup to avoid virtual dispatch during destruction
    m_Running = false;
    if (m_SamplerThread.joinable())
    {
        m_SamplerThread.request_stop();
        m_SamplerThread.join();
    }
    m_Model.reset();
}

void SystemMetricsPanel::onAttach()
{
    // Create system model with platform probe
    m_Model = std::make_unique<Domain::SystemModel>(Platform::makeSystemProbe());

    // Start background sampler
    m_Running = true;
    m_SamplerThread = std::jthread(
        [this](std::stop_token stopToken)
        {
            while (!stopToken.stop_requested() && m_Running)
            {
                m_Model->refresh();
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });

    spdlog::info("SystemMetricsPanel: initialized with background sampling");
}

void SystemMetricsPanel::onDetach()
{
    m_Running = false;
    if (m_SamplerThread.joinable())
    {
        m_SamplerThread.request_stop();
        m_SamplerThread.join();
    }
    m_Model.reset();
}

void SystemMetricsPanel::render(bool* open)
{
    if (!ImGui::Begin(m_Hostname.c_str(), open))
    {
        ImGui::End();
        return;
    }

    if (!m_Model)
    {
        const auto& theme = UI::Theme::get();
        ImGui::TextColored(theme.scheme().textError, "System model not initialized");
        ImGui::End();
        return;
    }

    // Get thread-safe copy of current snapshot
    auto snap = m_Model->snapshot();

    // Update cached hostname if changed
    if (!snap.hostname.empty() && snap.hostname != m_Hostname)
    {
        m_Hostname = snap.hostname;
    }

    // Delayed layout recalculation:
    // Frame N: Font changes externally
    // Frame N+1: We detect the change, mark layout dirty (but DON'T recalculate yet)
    // Frame N+2: Layout is dirty, so we recalculate (font is now stable)
    auto& theme = UI::Theme::get();
    auto currentFontSize = theme.currentFontSize();
    size_t currentCoreCount = snap.cpuPerCore.size();

    if (currentFontSize != m_LastFontSize || currentCoreCount != m_LastCoreCount)
    {
        // Font or core count changed - mark dirty for NEXT frame
        m_LastFontSize = currentFontSize;
        m_LastCoreCount = currentCoreCount;
        m_LayoutDirty = true;
    }
    else if (m_LayoutDirty)
    {
        // One frame has passed since change, now safe to recalculate
        updateCachedLayout();
        m_LayoutDirty = false;
    }

    // Tabs for different views
    if (ImGui::BeginTabBar("SystemTabs"))
    {
        if (ImGui::BeginTabItem("Overview"))
        {
            renderOverview();
            ImGui::EndTabItem();
        }

        if (snap.coreCount > 1)
        {
            if (ImGui::BeginTabItem("CPU Cores"))
            {
                renderPerCoreSection();
                ImGui::EndTabItem();
            }
        }

        if (ImGui::BeginTabItem("Memory"))
        {
            renderMemorySection();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

void SystemMetricsPanel::renderOverview()
{
    auto snap = m_Model->snapshot();

    // Header line: CPU Model | Cores | Freq | Uptime (right-aligned)
    // Format uptime string
    std::string uptimeStr;
    if (snap.uptimeSeconds > 0)
    {
        uint64_t days = snap.uptimeSeconds / 86400;
        uint64_t hours = (snap.uptimeSeconds % 86400) / 3600;
        uint64_t minutes = (snap.uptimeSeconds % 3600) / 60;

        char uptimeBuf[64];
        if (days > 0)
        {
            snprintf(uptimeBuf, sizeof(uptimeBuf), "Up: %lud %luh %lum",
                     static_cast<unsigned long>(days),
                     static_cast<unsigned long>(hours),
                     static_cast<unsigned long>(minutes));
        }
        else if (hours > 0)
        {
            snprintf(uptimeBuf, sizeof(uptimeBuf), "Up: %luh %lum",
                     static_cast<unsigned long>(hours),
                     static_cast<unsigned long>(minutes));
        }
        else
        {
            snprintf(uptimeBuf, sizeof(uptimeBuf), "Up: %lum", static_cast<unsigned long>(minutes));
        }
        uptimeStr = uptimeBuf;
    }

    // Display: "CPU Model (N cores @ X.XX GHz)     Uptime: Xd Yh Zm"
    char coreInfo[64];
    if (snap.cpuFreqMHz > 0)
    {
        snprintf(coreInfo, sizeof(coreInfo), " (%d cores @ %.2f GHz)",
                 snap.coreCount, static_cast<double>(snap.cpuFreqMHz) / 1000.0);
    }
    else
    {
        snprintf(coreInfo, sizeof(coreInfo), " (%d cores)", snap.coreCount);
    }

    float availWidth = ImGui::GetContentRegionAvail().x;
    float uptimeWidth = ImGui::CalcTextSize(uptimeStr.c_str()).x;

    // CPU model with core count and frequency
    ImGui::TextUnformatted(snap.cpuModel.c_str());
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(coreInfo);

    // Right-align uptime
    if (!uptimeStr.empty())
    {
        ImGui::SameLine(availWidth - uptimeWidth);
        ImGui::TextUnformatted(uptimeStr.c_str());
    }

    ImGui::Spacing();

    // Get theme for colored progress bars
    auto& theme = UI::Theme::get();

    // CPU usage bar with themed color
    ImGui::Text("CPU Usage:");
    ImGui::SameLine(m_OverviewLabelWidth);
    float cpuFraction = static_cast<float>(snap.cpuTotal.totalPercent) / 100.0F;
    char cpuOverlay[32];
    snprintf(cpuOverlay, sizeof(cpuOverlay), "%.1f%%", snap.cpuTotal.totalPercent);
    ImVec4 cpuColor = theme.progressColor(snap.cpuTotal.totalPercent);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, cpuColor);
    ImGui::ProgressBar(cpuFraction, ImVec2(-1, 0), cpuOverlay);
    ImGui::PopStyleColor();

    // CPU breakdown stacked bar (User | System | I/O Wait | Idle)
    {
        ImGui::Text("Breakdown:");
        ImGui::SameLine(m_OverviewLabelWidth);

        ImVec2 barStart = ImGui::GetCursorScreenPos();
        float barWidth = ImGui::GetContentRegionAvail().x;
        float barHeight = ImGui::GetFrameHeight();

        // Calculate segment widths based on percentages
        float userWidth = barWidth * static_cast<float>(snap.cpuTotal.userPercent) / 100.0F;
        float systemWidth = barWidth * static_cast<float>(snap.cpuTotal.systemPercent) / 100.0F;
        float iowaitWidth = barWidth * static_cast<float>(snap.cpuTotal.iowaitPercent) / 100.0F;
        // Idle is the background - drawn first, then overlaid with other segments

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Draw background
        drawList->AddRectFilled(
            barStart, ImVec2(barStart.x + barWidth, barStart.y + barHeight), ImGui::ColorConvertFloat4ToU32(theme.scheme().cpuIdle), 3.0F);

        float xOffset = 0.0F;

        // User segment
        if (userWidth > 0.5F)
        {
            drawList->AddRectFilled(ImVec2(barStart.x + xOffset, barStart.y),
                                    ImVec2(barStart.x + xOffset + userWidth, barStart.y + barHeight),
                                    ImGui::ColorConvertFloat4ToU32(theme.scheme().cpuUser),
                                    xOffset < 0.5F ? 3.0F : 0.0F,
                                    xOffset < 0.5F ? ImDrawFlags_RoundCornersLeft : 0);
            xOffset += userWidth;
        }

        // System segment
        if (systemWidth > 0.5F)
        {
            drawList->AddRectFilled(ImVec2(barStart.x + xOffset, barStart.y),
                                    ImVec2(barStart.x + xOffset + systemWidth, barStart.y + barHeight),
                                    ImGui::ColorConvertFloat4ToU32(theme.scheme().cpuSystem));
            xOffset += systemWidth;
        }

        // I/O Wait segment
        if (iowaitWidth > 0.5F)
        {
            drawList->AddRectFilled(ImVec2(barStart.x + xOffset, barStart.y),
                                    ImVec2(barStart.x + xOffset + iowaitWidth, barStart.y + barHeight),
                                    ImGui::ColorConvertFloat4ToU32(theme.scheme().cpuIowait));
            xOffset += iowaitWidth;
        }

        // Draw frame border
        drawList->AddRect(barStart,
                          ImVec2(barStart.x + barWidth, barStart.y + barHeight),
                          ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_Border)),
                          3.0F);

        // Reserve space for the bar
        ImGui::Dummy(ImVec2(barWidth, barHeight));

        // Tooltip on hover with breakdown details
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::TextColored(theme.scheme().cpuUser, "User: %.1f%%", snap.cpuTotal.userPercent);
            ImGui::TextColored(theme.scheme().cpuSystem, "System: %.1f%%", snap.cpuTotal.systemPercent);
            ImGui::TextColored(theme.scheme().cpuIowait, "I/O Wait: %.1f%%", snap.cpuTotal.iowaitPercent);
            ImGui::TextColored(theme.scheme().cpuIdle, "Idle: %.1f%%", snap.cpuTotal.idlePercent);
            ImGui::EndTooltip();
        }
    }

    // CPU History Graph
    {
        auto cpuHist = m_Model->cpuHistory();
        std::vector<float> timeData(cpuHist.size());
        for (size_t i = 0; i < cpuHist.size(); ++i)
        {
            timeData[i] = static_cast<float>(i) - static_cast<float>(cpuHist.size() - 1);
        }

        if (ImPlot::BeginPlot("##OverviewCPUHistory", ImVec2(-1, 120)))
        {
            ImPlot::SetupAxes(nullptr,
                              nullptr,
                              ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_NoTickLabels,
                              ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoTickLabels);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

            if (!cpuHist.empty())
            {
                ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                ImVec4 fillColor = theme.scheme().chartCpu;
                fillColor.w = 0.3F;
                ImPlot::SetNextFillStyle(fillColor);
                ImPlot::PlotLine("##CPU", timeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()));
                ImPlot::PlotShaded("##CPUShaded", timeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()), 0.0);
            }
            else
            {
                ImPlot::PlotDummy("##CPU");
            }

            ImPlot::EndPlot();
        }
    }

    ImGui::Spacing();

    // Load average (Linux only, shows nothing on Windows)
    if (snap.loadAvg1 > 0.0 || snap.loadAvg5 > 0.0 || snap.loadAvg15 > 0.0)
    {
        ImGui::Text("Load Avg:");
        ImGui::SameLine(m_OverviewLabelWidth);

        char loadBuf[64];
        snprintf(loadBuf, sizeof(loadBuf), "%.2f / %.2f / %.2f (1/5/15 min)",
                 snap.loadAvg1, snap.loadAvg5, snap.loadAvg15);
        ImGui::TextUnformatted(loadBuf);
    }

    // Memory usage bar with themed color
    ImGui::Text("Memory:");
    ImGui::SameLine(m_OverviewLabelWidth);
    float memFraction = static_cast<float>(snap.memoryUsedPercent) / 100.0F;

    double usedGB = static_cast<double>(snap.memoryUsedBytes) / (1024.0 * 1024.0 * 1024.0);
    double totalGB = static_cast<double>(snap.memoryTotalBytes) / (1024.0 * 1024.0 * 1024.0);
    char memOverlay[64];
    snprintf(memOverlay, sizeof(memOverlay), "%.1f / %.1f GB (%.1f%%)", usedGB, totalGB, snap.memoryUsedPercent);
    ImVec4 memColor = theme.progressColor(snap.memoryUsedPercent);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, memColor);
    ImGui::ProgressBar(memFraction, ImVec2(-1, 0), memOverlay);
    ImGui::PopStyleColor();

    // Swap usage bar (if available) with themed color
    if (snap.swapTotalBytes > 0)
    {
        ImGui::Text("Swap:");
        ImGui::SameLine(m_OverviewLabelWidth);
        float swapFraction = static_cast<float>(snap.swapUsedPercent) / 100.0F;

        double swapUsedGB = static_cast<double>(snap.swapUsedBytes) / (1024.0 * 1024.0 * 1024.0);
        double swapTotalGB = static_cast<double>(snap.swapTotalBytes) / (1024.0 * 1024.0 * 1024.0);
        char swapOverlay[64];
        snprintf(swapOverlay, sizeof(swapOverlay), "%.1f / %.1f GB (%.1f%%)", swapUsedGB, swapTotalGB, snap.swapUsedPercent);
        ImVec4 swapColor = theme.progressColor(snap.swapUsedPercent);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, swapColor);
        ImGui::ProgressBar(swapFraction, ImVec2(-1, 0), swapOverlay);
        ImGui::PopStyleColor();
    }
}

void SystemMetricsPanel::renderCpuSection()
{
    const auto& theme = UI::Theme::get();
    auto snap = m_Model->snapshot();
    auto cpuHist = m_Model->cpuHistory();

    ImGui::Text("CPU History (last %zu samples)", cpuHist.size());
    ImGui::Spacing();

    // Prepare time axis
    std::vector<float> timeData(cpuHist.size());
    for (size_t i = 0; i < cpuHist.size(); ++i)
    {
        timeData[i] = static_cast<float>(i) - static_cast<float>(cpuHist.size() - 1);
    }

    // CPU Usage Plot
    if (ImPlot::BeginPlot("##CPUHistory", ImVec2(-1, 200)))
    {
        ImPlot::SetupAxes("Time (s)", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

        if (!cpuHist.empty())
        {
            ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
            ImVec4 fillColor = theme.scheme().chartCpu;
            fillColor.w = 0.3F; // Semi-transparent fill
            ImPlot::SetNextFillStyle(fillColor);
            ImPlot::PlotLine("##CPU", timeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()));
            ImPlot::PlotShaded("##CPUShaded", timeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()), 0.0);
        }
        else
        {
            ImPlot::PlotDummy("##CPU");
        }

        ImPlot::EndPlot();
    }

    ImGui::Spacing();

    // Current values
    ImGui::Text("Current: %.1f%% (User: %.1f%%, System: %.1f%%)",
                snap.cpuTotal.totalPercent,
                snap.cpuTotal.userPercent,
                snap.cpuTotal.systemPercent);
}

void SystemMetricsPanel::renderMemorySection()
{
    const auto& theme = UI::Theme::get();
    auto snap = m_Model->snapshot();
    auto memHist = m_Model->memoryHistory();
    auto swapHist = m_Model->swapHistory();

    ImGui::Text("Memory History (last %zu samples)", memHist.size());
    ImGui::Spacing();

    // Prepare time axis
    std::vector<float> timeData(memHist.size());
    for (size_t i = 0; i < memHist.size(); ++i)
    {
        timeData[i] = static_cast<float>(i) - static_cast<float>(memHist.size() - 1);
    }

    // Memory Usage Plot
    if (ImPlot::BeginPlot("##MemoryHistory", ImVec2(-1, 200)))
    {
        ImPlot::SetupAxes("Time (s)", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

        if (!memHist.empty())
        {
            ImPlot::SetNextLineStyle(theme.scheme().chartMemory, 2.0F);
            ImVec4 fillColor = theme.scheme().chartMemory;
            fillColor.w = 0.3F; // Semi-transparent fill
            ImPlot::SetNextFillStyle(fillColor);
            ImPlot::PlotLine("##Memory", timeData.data(), memHist.data(), static_cast<int>(memHist.size()));
            ImPlot::PlotShaded("##MemoryShaded", timeData.data(), memHist.data(), static_cast<int>(memHist.size()), 0.0);
        }
        else
        {
            ImPlot::PlotDummy("##Memory");
        }

        ImPlot::EndPlot();
    }

    ImGui::Spacing();

    // Memory details
    auto formatSize = [](uint64_t bytes) -> std::pair<double, const char*>
    {
        constexpr double GB = 1024.0 * 1024.0 * 1024.0;
        constexpr double MB = 1024.0 * 1024.0;

        double value = static_cast<double>(bytes);
        if (value >= GB)
        {
            return {value / GB, "GB"};
        }
        return {value / MB, "MB"};
    };

    auto [usedVal, usedUnit] = formatSize(snap.memoryUsedBytes);
    auto [totalVal, totalUnit] = formatSize(snap.memoryTotalBytes);
    auto [availVal, availUnit] = formatSize(snap.memoryAvailableBytes);
    auto [cachedVal, cachedUnit] = formatSize(snap.memoryCachedBytes);

    ImGui::Text("Used: %.1f %s / %.1f %s (%.1f%%)", usedVal, usedUnit, totalVal, totalUnit, snap.memoryUsedPercent);
    ImGui::Text("Available: %.1f %s", availVal, availUnit);
    ImGui::Text("Cached: %.1f %s", cachedVal, cachedUnit);

    // Swap section
    if (snap.swapTotalBytes > 0)
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Swap History");

        std::vector<float> swapTimeData(swapHist.size());
        for (size_t i = 0; i < swapHist.size(); ++i)
        {
            swapTimeData[i] = static_cast<float>(i) - static_cast<float>(swapHist.size() - 1);
        }

        if (ImPlot::BeginPlot("##SwapHistory", ImVec2(-1, 150)))
        {
            ImPlot::SetupAxes("Time (s)", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_Lock);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

            if (!swapHist.empty())
            {
                ImPlot::SetNextLineStyle(theme.scheme().chartIo, 2.0F);
                ImPlot::PlotLine("##Swap", swapTimeData.data(), swapHist.data(), static_cast<int>(swapHist.size()));
            }
            else
            {
                ImPlot::PlotDummy("##Swap");
            }

            ImPlot::EndPlot();
        }

        auto [swapUsedVal, swapUsedUnit] = formatSize(snap.swapUsedBytes);
        auto [swapTotalVal, swapTotalUnit] = formatSize(snap.swapTotalBytes);
        ImGui::Text("Swap Used: %.1f %s / %.1f %s (%.1f%%)", swapUsedVal, swapUsedUnit, swapTotalVal, swapTotalUnit, snap.swapUsedPercent);
    }
}

void SystemMetricsPanel::renderPerCoreSection()
{
    auto snap = m_Model->snapshot();
    auto perCoreHist = m_Model->perCoreHistory();
    auto& theme = UI::Theme::get();

    ImGui::Text("Per-Core CPU Usage (%d cores)", snap.coreCount);
    ImGui::Spacing();

    // ========================================
    // Multi-column progress bars
    // ========================================
    const size_t numCores = snap.cpuPerCore.size();
    if (numCores == 0)
    {
        ImGui::TextColored(theme.scheme().textMuted, "No per-core data available");
        return;
    }

    // Calculate optimal column count based on core count and window width
    float windowWidth = ImGui::GetContentRegionAvail().x;
    constexpr float minColWidth = 200.0F; // Minimum width per column
    int numCols = std::max(1, std::min(4, static_cast<int>(windowWidth / minColWidth)));
    int numRows = static_cast<int>((numCores + static_cast<size_t>(numCols) - 1) / static_cast<size_t>(numCols));

    if (ImGui::BeginTable("PerCoreBars", numCols, ImGuiTableFlags_SizingStretchSame))
    {
        for (int row = 0; row < numRows; ++row)
        {
            ImGui::TableNextRow();
            for (int col = 0; col < numCols; ++col)
            {
                size_t coreIdx = static_cast<size_t>(row * numCols + col);
                if (coreIdx >= numCores)
                {
                    break;
                }

                ImGui::TableNextColumn();

                double percent = snap.cpuPerCore[coreIdx].totalPercent;
                float fraction = static_cast<float>(percent) / 100.0F;

                char overlay[16];
                snprintf(overlay, sizeof(overlay), "%5.1f%%", percent);

                // Use theme color
                ImVec4 color = theme.progressColor(percent);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);

                // Right-aligned label within cached width
                char label[8];
                snprintf(label, sizeof(label), "%zu", coreIdx);
                float labelW = ImGui::CalcTextSize(label).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + m_PerCoreLabelWidth - labelW);
                ImGui::Text("%s", label);
                ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x);
                ImGui::ProgressBar(fraction, ImVec2(-1, 0), overlay);

                ImGui::PopStyleColor();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ========================================
    // Heatmap for history
    // ========================================
    ImGui::Text("Per-Core History (Heatmap)");

    if (!perCoreHist.empty() && !perCoreHist[0].empty())
    {
        size_t historySize = perCoreHist[0].size();
        size_t coreCount = perCoreHist.size();

        // Build heatmap data (row-major: cores × time samples)
        std::vector<double> heatmapData(coreCount * historySize);
        for (size_t core = 0; core < coreCount; ++core)
        {
            for (size_t t = 0; t < historySize; ++t)
            {
                // ImPlot heatmap expects row-major with (0,0) at top-left
                // We want oldest time on left, newest on right
                // And core 0 at top, highest core at bottom
                heatmapData[core * historySize + t] = static_cast<double>(perCoreHist[core][t]);
            }
        }

        // Use remaining vertical space in the panel, with minimum height
        float availableHeight = ImGui::GetContentRegionAvail().y;
        float heatmapHeight = std::max(availableHeight, 80.0F);

        // Set up colormap based on theme (only recreate when theme changes)
        std::size_t currentThemeIdx = theme.currentThemeIndex();
        if (m_HeatmapColormap == -1 || currentThemeIdx != m_LastThemeIndex)
        {
            const auto& hm = theme.scheme().heatmap;
            ImVec4 colors[5] = {hm[0], hm[1], hm[2], hm[3], hm[4]};

            // Generate unique name for this theme's colormap
            char cmapName[32];
            snprintf(cmapName, sizeof(cmapName), "CPUHeat_%zu", currentThemeIdx);

            // Check if this colormap already exists
            int existingIdx = ImPlot::GetColormapIndex(cmapName);
            if (existingIdx == -1)
            {
                m_HeatmapColormap = ImPlot::AddColormap(cmapName, &colors[0], 5, true);
            }
            else
            {
                m_HeatmapColormap = existingIdx;
            }
            m_LastThemeIndex = currentThemeIdx;
        }

        ImPlot::PushColormap(m_HeatmapColormap);

        if (ImPlot::BeginPlot("##CPUHeatmap", ImVec2(-1, heatmapHeight), 
                              ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText | ImPlotFlags_NoFrame))
        {
            // No axis labels - cleaner look
            ImPlot::SetupAxes(nullptr, nullptr, 
                              ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks,
                              ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks);
            
            // Set limits
            ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(historySize), ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, -0.5, static_cast<double>(coreCount) - 0.5, ImPlotCond_Always);

            double xMin = 0.0;
            double xMax = static_cast<double>(historySize);
            double yMin = -0.5;
            double yMax = static_cast<double>(coreCount) - 0.5;

            // heatmapData already set up above with correct order:
            // History buffer: index 0 = oldest, index size-1 = newest
            // Direct copy means: left side = oldest, right side = newest

            ImPlot::PlotHeatmap("##heat",
                                heatmapData.data(),
                                static_cast<int>(coreCount),
                                static_cast<int>(historySize),
                                0.0,   // scale min
                                100.0, // scale max
                                nullptr,
                                ImPlotPoint(xMin, yMin),
                                ImPlotPoint(xMax, yMax));

            ImPlot::EndPlot();
        }

        ImPlot::PopColormap();
        
        // Simple description below the heatmap
        ImGui::TextColored(theme.scheme().textMuted, "Cores 0-%zu (top to bottom)  |  Oldest (left) to Now (right)", 
                           coreCount - 1);
    }
    else
    {
        ImGui::TextColored(theme.scheme().textMuted, "Collecting data...");
    }
}

void SystemMetricsPanel::updateCachedLayout()
{
    auto& theme = UI::Theme::get();

    // Overview: width needed for "CPU Usage:" label + spacing
    m_OverviewLabelWidth = ImGui::CalcTextSize("CPU Usage:").x + ImGui::GetStyle().ItemSpacing.x;

    // Per-core: width needed for max core number (e.g., "31" for 32 cores)
    if (m_LastCoreCount > 0)
    {
        char maxLabel[8];
        snprintf(maxLabel, sizeof(maxLabel), "%zu", m_LastCoreCount - 1);
        m_PerCoreLabelWidth = ImGui::CalcTextSize(maxLabel).x;
    }
    else
    {
        m_PerCoreLabelWidth = ImGui::CalcTextSize("0").x;
    }

    spdlog::debug("SystemMetricsPanel: cached layout updated (font={}, overviewWidth={:.1f}, perCoreWidth={:.1f})",
                  static_cast<int>(theme.currentFontSize()),
                  m_OverviewLabelWidth,
                  m_PerCoreLabelWidth);
}

} // namespace App
