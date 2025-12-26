#include "StoragePanel.h"

#include "App/Panel.h"
#include "App/UserConfig.h"
#include "Domain/StorageModel.h"
#include "Platform/Factory.h"
#include "UI/Format.h"
#include "UI/HistoryWidgets.h"
#include "UI/Numeric.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace App
{

namespace
{

using UI::Widgets::smoothTowards;

void drawProgressBarWithOverlay(double fraction01, const std::string& overlay, const ImVec4& color)
{
    const double clamped = std::clamp(fraction01, 0.0, 1.0);
    const float fraction = UI::Numeric::toFloatNarrow(clamped);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), "");
    UI::Widgets::drawRightAlignedOverlayText(overlay.c_str());
    ImGui::PopStyleColor();
}

[[nodiscard]] std::string formatBytesPerSecond(double bytesPerSec)
{
    // Convert to uint64_t for formatting (clamped to valid range)
    const auto bytes = static_cast<uint64_t>(std::max(0.0, bytesPerSec));
    const auto unit = UI::Format::unitForTotalBytes(bytes);
    return UI::Format::formatBytesWithUnit(static_cast<double>(bytes), unit);
}

} // namespace

StoragePanel::StoragePanel() : Panel("Storage")
{
}

StoragePanel::~StoragePanel() = default;

void StoragePanel::onAttach()
{
    spdlog::info("StoragePanel: attaching");
    m_Model = std::make_unique<Domain::StorageModel>(Platform::makeDiskProbe());
    m_Model->setMaxHistorySeconds(m_MaxHistorySeconds);

    // Initial sample
    m_Model->sample();

    spdlog::info("StoragePanel: attached");
}

void StoragePanel::onDetach()
{
    spdlog::info("StoragePanel: detaching");
    m_Model.reset();
}

void StoragePanel::onUpdate(float deltaTime)
{
    if (!m_Model)
    {
        return;
    }

    m_RefreshAccumulatorSec += deltaTime;
    const float intervalSec = static_cast<float>(m_RefreshInterval.count()) / 1000.0F;

    if (m_ForceRefresh || m_RefreshAccumulatorSec >= intervalSec)
    {
        m_Model->sample();
        m_RefreshAccumulatorSec = 0.0F;
        m_ForceRefresh = false;
    }

    // Update smoothed metrics for display
    const auto snap = m_Model->latestSnapshot();
    updateSmoothedMetrics(snap, deltaTime);
}

void StoragePanel::setSamplingInterval(std::chrono::milliseconds interval)
{
    m_RefreshInterval = interval;
}

void StoragePanel::requestRefresh()
{
    m_ForceRefresh = true;
}

void StoragePanel::render(bool* open)
{
    if (!ImGui::Begin("Storage", open, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return;
    }

    // Check if font size changed
    const auto currentFontSize = UI::Theme::get().currentFontSize();
    if (currentFontSize != m_LastFontSize)
    {
        m_LastFontSize = currentFontSize;
        m_LayoutDirty = true;
    }

    if (m_LayoutDirty)
    {
        updateCachedLayout();
        m_LayoutDirty = false;
    }

    if (!m_Model)
    {
        ImGui::TextDisabled("Storage model not initialized");
        ImGui::End();
        return;
    }

    const auto snap = m_Model->latestSnapshot();
    const auto caps = m_Model->capabilities();

    if (!caps.hasDiskStats)
    {
        ImGui::TextDisabled("Disk statistics not available on this platform");
        ImGui::End();
        return;
    }

    renderOverview();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    renderDeviceDetails();

    ImGui::End();
}

void StoragePanel::renderOverview()
{
    ImGui::SeparatorText("Overview");

    const auto snap = m_Model->latestSnapshot();
    const auto& theme = UI::Theme::get();
    const auto& scheme = theme.scheme();

    // System-wide totals
    ImGui::Text("Total Read:");
    ImGui::SameLine(m_OverviewLabelWidth);
    const std::string readStr = formatBytesPerSecond(snap.totalReadBytesPerSec) + "/s";
    ImGui::TextColored(scheme.chartCpu, "%s", readStr.c_str());

    ImGui::Text("Total Write:");
    ImGui::SameLine(m_OverviewLabelWidth);
    const std::string writeStr = formatBytesPerSecond(snap.totalWriteBytesPerSec) + "/s";
    ImGui::TextColored(scheme.chartIo, "%s", writeStr.c_str());

    ImGui::Text("Devices:");
    ImGui::SameLine(m_OverviewLabelWidth);
    ImGui::Text("%zu", snap.disks.size());

    ImGui::Text("Total Ops:");
    ImGui::SameLine(m_OverviewLabelWidth);
    ImGui::Text("%.1f reads/s, %.1f writes/s", snap.totalReadOpsPerSec, snap.totalWriteOpsPerSec);
}

void StoragePanel::renderDeviceDetails()
{
    ImGui::SeparatorText("Devices");

    const auto snap = m_Model->latestSnapshot();
    const auto& theme = UI::Theme::get();
    const auto& scheme = theme.scheme();

    if (snap.disks.empty())
    {
        ImGui::TextDisabled("No disk devices found");
        return;
    }

    // Create a table for device details
    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("DiskTable", 6, tableFlags))
    {
        ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthFixed, 100.0F);
        ImGui::TableSetupColumn("Read", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Write", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Utilization", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Read Ops/s", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        ImGui::TableSetupColumn("Write Ops/s", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        ImGui::TableHeadersRow();

        for (const auto& disk : snap.disks)
        {
            ImGui::TableNextRow();

            // Device name
            ImGui::TableNextColumn();
            ImGui::Text("%s", disk.deviceName.c_str());

            // Read rate
            ImGui::TableNextColumn();
            const auto& smoothed = m_SmoothedDisks[disk.deviceName];
            if (smoothed.initialized)
            {
                const std::string readStr = formatBytesPerSecond(smoothed.readMBps * 1024.0 * 1024.0) + "/s";
                ImGui::TextColored(scheme.chartCpu, "%s", readStr.c_str());
            }
            else
            {
                const std::string readStr = formatBytesPerSecond(disk.readBytesPerSec) + "/s";
                ImGui::Text("%s", readStr.c_str());
            }

            // Write rate
            ImGui::TableNextColumn();
            if (smoothed.initialized)
            {
                const std::string writeStr = formatBytesPerSecond(smoothed.writeMBps * 1024.0 * 1024.0) + "/s";
                ImGui::TextColored(scheme.chartIo, "%s", writeStr.c_str());
            }
            else
            {
                const std::string writeStr = formatBytesPerSecond(disk.writeBytesPerSec) + "/s";
                ImGui::Text("%s", writeStr.c_str());
            }

            // Utilization
            ImGui::TableNextColumn();
            if (snap.hasIoTime)
            {
                const double util = smoothed.initialized ? smoothed.utilization : disk.utilizationPercent;
                const std::string utilStr = std::format("{:.1f}%", util);

                ImVec4 utilColor = theme.progressColor(util);

                drawProgressBarWithOverlay(util / 100.0, utilStr, utilColor);
            }
            else
            {
                ImGui::TextDisabled("N/A");
            }

            // Read ops/s
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", disk.readOpsPerSec);

            // Write ops/s
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", disk.writeOpsPerSec);
        }

        ImGui::EndTable();
    }
}

void StoragePanel::updateCachedLayout()
{
    // Measure label widths for proper alignment
    m_OverviewLabelWidth = 0.0F;
    // TODO: Store labels as std::string_view
    const std::array<const char*, 4> labels = {"Total Read:", "Total Write:", "Devices:", "Total Ops:"};
    for (const char* label : labels)
    {
        const float w = ImGui::CalcTextSize(label).x;
        m_OverviewLabelWidth = std::max(m_OverviewLabelWidth, w);
    }
    m_OverviewLabelWidth += ImGui::GetStyle().ItemSpacing.x;
}

void StoragePanel::updateSmoothedMetrics(const Domain::StorageSnapshot& snap, float deltaTimeSeconds)
{
    // Use the refresh interval for smoothing calculation
    const auto refreshInterval = m_RefreshInterval;
    const double alpha = UI::Widgets::computeAlpha(deltaTimeSeconds, refreshInterval);

    for (const auto& disk : snap.disks)
    {
        auto& smoothed = m_SmoothedDisks[disk.deviceName];

        const double readMBps = disk.readBytesPerSec / (1024.0 * 1024.0);
        const double writeMBps = disk.writeBytesPerSec / (1024.0 * 1024.0);

        if (!smoothed.initialized)
        {
            smoothed.readMBps = readMBps;
            smoothed.writeMBps = writeMBps;
            smoothed.utilization = disk.utilizationPercent;
            smoothed.initialized = true;
        }
        else
        {
            smoothed.readMBps = smoothTowards(smoothed.readMBps, readMBps, alpha);
            smoothed.writeMBps = smoothTowards(smoothed.writeMBps, writeMBps, alpha);
            smoothed.utilization = smoothTowards(smoothed.utilization, disk.utilizationPercent, alpha);
        }
    }
}

} // namespace App
