#include "SystemMetricsPanel.h"

#include "Platform/Factory.h"

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>

#include <imgui.h>
#include <implot.h>

namespace App
{

SystemMetricsPanel::SystemMetricsPanel() : Panel("System Metrics")
{
}

SystemMetricsPanel::~SystemMetricsPanel()
{
    onDetach();
}

void SystemMetricsPanel::onAttach()
{
    // Create system model with platform probe
    m_Model = std::make_unique<Domain::SystemModel>(Platform::makeSystemProbe());

    // Start background sampler
    m_Running = true;
    m_SamplerThread = std::jthread([this](std::stop_token stopToken) {
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
    if (!ImGui::Begin("System Metrics", open))
    {
        ImGui::End();
        return;
    }

    if (!m_Model)
    {
        ImGui::TextColored(ImVec4(1.0F, 0.3F, 0.3F, 1.0F), "System model not initialized");
        ImGui::End();
        return;
    }

    // Get thread-safe copy of current snapshot
    auto snap = m_Model->snapshot();

    // Tabs for different views
    if (ImGui::BeginTabBar("SystemTabs"))
    {
        if (ImGui::BeginTabItem("Overview"))
        {
            renderOverview();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("CPU"))
        {
            renderCpuSection();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Memory"))
        {
            renderMemorySection();
            ImGui::EndTabItem();
        }

        if (snap.coreCount > 1)
        {
            if (ImGui::BeginTabItem("Per-Core"))
            {
                renderPerCoreSection();
                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

void SystemMetricsPanel::renderOverview()
{
    auto snap = m_Model->snapshot();

    // System summary
    ImGui::Text("System Overview");
    ImGui::Separator();
    ImGui::Spacing();

    // Uptime
    if (snap.uptimeSeconds > 0)
    {
        uint64_t days = snap.uptimeSeconds / 86400;
        uint64_t hours = (snap.uptimeSeconds % 86400) / 3600;
        uint64_t minutes = (snap.uptimeSeconds % 3600) / 60;

        if (days > 0)
        {
            ImGui::Text("Uptime: %lud %luh %lum", static_cast<unsigned long>(days),
                        static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
        }
        else if (hours > 0)
        {
            ImGui::Text("Uptime: %luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
        }
        else
        {
            ImGui::Text("Uptime: %lum", static_cast<unsigned long>(minutes));
        }
    }

    ImGui::Text("CPU Cores: %d", snap.coreCount);
    ImGui::Spacing();

    // CPU usage bar
    ImGui::Text("CPU Usage:");
    ImGui::SameLine(100.0F);
    float cpuFraction = static_cast<float>(snap.cpuTotal.totalPercent) / 100.0F;
    char cpuOverlay[32];
    snprintf(cpuOverlay, sizeof(cpuOverlay), "%.1f%%", snap.cpuTotal.totalPercent);
    ImGui::ProgressBar(cpuFraction, ImVec2(-1, 0), cpuOverlay);

    // Memory usage bar
    ImGui::Text("Memory:");
    ImGui::SameLine(100.0F);
    float memFraction = static_cast<float>(snap.memoryUsedPercent) / 100.0F;

    double usedGB = static_cast<double>(snap.memoryUsedBytes) / (1024.0 * 1024.0 * 1024.0);
    double totalGB = static_cast<double>(snap.memoryTotalBytes) / (1024.0 * 1024.0 * 1024.0);
    char memOverlay[64];
    snprintf(memOverlay, sizeof(memOverlay), "%.1f / %.1f GB (%.1f%%)", usedGB, totalGB, snap.memoryUsedPercent);
    ImGui::ProgressBar(memFraction, ImVec2(-1, 0), memOverlay);

    // Swap usage bar (if available)
    if (snap.swapTotalBytes > 0)
    {
        ImGui::Text("Swap:");
        ImGui::SameLine(100.0F);
        float swapFraction = static_cast<float>(snap.swapUsedPercent) / 100.0F;

        double swapUsedGB = static_cast<double>(snap.swapUsedBytes) / (1024.0 * 1024.0 * 1024.0);
        double swapTotalGB = static_cast<double>(snap.swapTotalBytes) / (1024.0 * 1024.0 * 1024.0);
        char swapOverlay[64];
        snprintf(swapOverlay, sizeof(swapOverlay), "%.1f / %.1f GB (%.1f%%)", swapUsedGB, swapTotalGB, snap.swapUsedPercent);
        ImGui::ProgressBar(swapFraction, ImVec2(-1, 0), swapOverlay);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // CPU breakdown
    ImGui::Text("CPU Breakdown:");
    ImGui::Spacing();

    if (ImGui::BeginTable("CpuBreakdown", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0F);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        auto addRow = [](const char* label, double value, ImVec4 color) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.7F, 0.7F, 0.7F, 1.0F), "%s", label);
            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%.1f%%", value);
        };

        addRow("User", snap.cpuTotal.userPercent, ImVec4(0.3F, 0.7F, 1.0F, 1.0F));
        addRow("System", snap.cpuTotal.systemPercent, ImVec4(1.0F, 0.5F, 0.3F, 1.0F));
        addRow("I/O Wait", snap.cpuTotal.iowaitPercent, ImVec4(1.0F, 0.8F, 0.3F, 1.0F));
        addRow("Idle", snap.cpuTotal.idlePercent, ImVec4(0.5F, 0.5F, 0.5F, 1.0F));

        if (snap.cpuTotal.stealPercent > 0.1)
        {
            addRow("Steal", snap.cpuTotal.stealPercent, ImVec4(1.0F, 0.3F, 0.3F, 1.0F));
        }

        ImGui::EndTable();
    }
}

void SystemMetricsPanel::renderCpuSection()
{
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
            ImPlot::SetNextLineStyle(ImVec4(0.3F, 0.7F, 1.0F, 1.0F), 2.0F);
            ImPlot::SetNextFillStyle(ImVec4(0.3F, 0.7F, 1.0F, 0.3F));
            ImPlot::PlotLine("CPU", timeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()));
            ImPlot::PlotShaded("CPU", timeData.data(), cpuHist.data(), static_cast<int>(cpuHist.size()), 0.0);
        }
        else
        {
            ImPlot::PlotDummy("CPU");
        }

        ImPlot::EndPlot();
    }

    ImGui::Spacing();

    // Current values
    ImGui::Text("Current: %.1f%% (User: %.1f%%, System: %.1f%%)",
                snap.cpuTotal.totalPercent, snap.cpuTotal.userPercent, snap.cpuTotal.systemPercent);
}

void SystemMetricsPanel::renderMemorySection()
{
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
            ImPlot::SetNextLineStyle(ImVec4(0.3F, 1.0F, 0.3F, 1.0F), 2.0F);
            ImPlot::SetNextFillStyle(ImVec4(0.3F, 1.0F, 0.3F, 0.3F));
            ImPlot::PlotLine("Memory", timeData.data(), memHist.data(), static_cast<int>(memHist.size()));
            ImPlot::PlotShaded("Memory", timeData.data(), memHist.data(), static_cast<int>(memHist.size()), 0.0);
        }
        else
        {
            ImPlot::PlotDummy("Memory");
        }

        ImPlot::EndPlot();
    }

    ImGui::Spacing();

    // Memory details
    auto formatSize = [](uint64_t bytes) -> std::pair<double, const char*> {
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
                ImPlot::SetNextLineStyle(ImVec4(1.0F, 0.5F, 0.3F, 1.0F), 2.0F);
                ImPlot::PlotLine("Swap", swapTimeData.data(), swapHist.data(), static_cast<int>(swapHist.size()));
            }
            else
            {
                ImPlot::PlotDummy("Swap");
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

    ImGui::Text("Per-Core CPU Usage (%d cores)", snap.coreCount);
    ImGui::Spacing();

    // Current usage bars for each core
    for (size_t i = 0; i < snap.cpuPerCore.size(); ++i)
    {
        char label[32];
        snprintf(label, sizeof(label), "Core %zu:", i);
        ImGui::Text("%s", label);
        ImGui::SameLine(70.0F);

        float fraction = static_cast<float>(snap.cpuPerCore[i].totalPercent) / 100.0F;
        char overlay[32];
        snprintf(overlay, sizeof(overlay), "%.1f%%", snap.cpuPerCore[i].totalPercent);

        // Color based on usage
        ImVec4 color;
        if (snap.cpuPerCore[i].totalPercent > 80.0)
        {
            color = ImVec4(1.0F, 0.3F, 0.3F, 1.0F); // Red for high usage
        }
        else if (snap.cpuPerCore[i].totalPercent > 50.0)
        {
            color = ImVec4(1.0F, 0.8F, 0.3F, 1.0F); // Yellow for medium
        }
        else
        {
            color = ImVec4(0.3F, 1.0F, 0.3F, 1.0F); // Green for low
        }

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
        ImGui::ProgressBar(fraction, ImVec2(-1, 0), overlay);
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Per-core history graph (stacked or overlaid)
    ImGui::Text("Per-Core History");

    if (!perCoreHist.empty() && !perCoreHist[0].empty())
    {
        std::vector<float> timeData(perCoreHist[0].size());
        for (size_t i = 0; i < timeData.size(); ++i)
        {
            timeData[i] = static_cast<float>(i) - static_cast<float>(timeData.size() - 1);
        }

        if (ImPlot::BeginPlot("##PerCoreHistory", ImVec2(-1, 250)))
        {
            ImPlot::SetupAxes("Time (s)", "%", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_Lock);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);

            // Color palette for cores
            static const ImVec4 coreColors[] = {
                ImVec4(0.3F, 0.7F, 1.0F, 1.0F),  // Blue
                ImVec4(1.0F, 0.5F, 0.3F, 1.0F),  // Orange
                ImVec4(0.3F, 1.0F, 0.3F, 1.0F),  // Green
                ImVec4(1.0F, 0.3F, 0.7F, 1.0F),  // Pink
                ImVec4(0.7F, 0.3F, 1.0F, 1.0F),  // Purple
                ImVec4(1.0F, 1.0F, 0.3F, 1.0F),  // Yellow
                ImVec4(0.3F, 1.0F, 1.0F, 1.0F),  // Cyan
                ImVec4(1.0F, 0.7F, 0.5F, 1.0F),  // Peach
            };
            constexpr size_t numColors = sizeof(coreColors) / sizeof(coreColors[0]);

            for (size_t core = 0; core < perCoreHist.size(); ++core)
            {
                char coreLabel[16];
                snprintf(coreLabel, sizeof(coreLabel), "Core %zu", core);

                ImPlot::SetNextLineStyle(coreColors[core % numColors], 1.5F);
                ImPlot::PlotLine(coreLabel, timeData.data(), perCoreHist[core].data(), static_cast<int>(perCoreHist[core].size()));
            }

            ImPlot::EndPlot();
        }
    }
    else
    {
        ImGui::TextColored(ImVec4(0.6F, 0.6F, 0.6F, 1.0F), "Collecting data...");
    }
}

} // namespace App
