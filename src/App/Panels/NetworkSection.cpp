#include "NetworkSection.h"

#include "App/Panels/NetInterfaceUtils.h"
#include "App/Panels/StorageSection.h"
#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace App::NetworkSection
{

namespace
{

using UI::Widgets::buildTimeAxis;
using UI::Widgets::computeAlpha;
using UI::Widgets::formatAgeSeconds;
using UI::Widgets::formatAxisBytesPerSec;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::smoothTowards;
using UI::Widgets::X_AXIS_FLAGS_DEFAULT;
using UI::Widgets::Y_AXIS_FLAGS_DEFAULT;

/// Update smoothed network values
void updateSmoothedNetwork(double targetSent, double targetRecv, float deltaTimeSeconds, RenderContext& ctx)
{
    if (ctx.smoothedNetSentBytesPerSec == nullptr || ctx.smoothedNetRecvBytesPerSec == nullptr || ctx.smoothedNetInitialized == nullptr)
    {
        return;
    }

    const double alpha = computeAlpha(deltaTimeSeconds, ctx.refreshInterval);

    if (!*ctx.smoothedNetInitialized)
    {
        *ctx.smoothedNetSentBytesPerSec = targetSent;
        *ctx.smoothedNetRecvBytesPerSec = targetRecv;
        *ctx.smoothedNetInitialized = true;
        return;
    }

    *ctx.smoothedNetSentBytesPerSec = smoothTowards(*ctx.smoothedNetSentBytesPerSec, targetSent, alpha);
    *ctx.smoothedNetRecvBytesPerSec = smoothTowards(*ctx.smoothedNetRecvBytesPerSec, targetRecv, alpha);
}

} // namespace

void renderDiskIOSection(RenderContext& ctx)
{
    // Delegate to StorageSection - this wrapper maintains API compatibility
    StorageSection::RenderContext storageCtx{
        .storageModel = ctx.storageModel,
        .maxHistorySeconds = ctx.maxHistorySeconds,
        .historyScrollSeconds = ctx.historyScrollSeconds,
        .lastDeltaSeconds = ctx.lastDeltaSeconds,
        .refreshInterval = ctx.refreshInterval,
        .smoothedReadBytesPerSec = ctx.smoothedDiskReadBytesPerSec,
        .smoothedWriteBytesPerSec = ctx.smoothedDiskWriteBytesPerSec,
        .smoothedInitialized = ctx.smoothedDiskInitialized,
    };
    StorageSection::renderStorageSection(storageCtx);
}

void renderNetworkSection(RenderContext& ctx)
{
    // Render Disk I/O section at the top of Network and I/O tab
    renderDiskIOSection(ctx);
    ImGui::Separator();
    ImGui::Spacing();

    const auto& theme = UI::Theme::get();
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    if (ctx.systemModel == nullptr || !ctx.systemModel->capabilities().hasNetworkCounters)
    {
        ImGui::TextUnformatted("Network monitoring not available on this platform.");
        return;
    }

    const auto netSnap = ctx.systemModel->snapshot();
    const auto& interfaces = netSnap.networkInterfaces;

    // Build interface selector dropdown
    std::vector<std::string> interfaceNames;
    interfaceNames.emplace_back("Total (All Interfaces)");
    for (const auto& iface : interfaces)
    {
        // Use display name if available, otherwise interface name
        interfaceNames.push_back(iface.displayName.empty() ? iface.name : iface.displayName);
    }

    const auto interfaceCount = interfaces.size();

    // Get selected interface (or default to -1)
    int selectedInterface = (ctx.selectedNetworkInterface != nullptr) ? *ctx.selectedNetworkInterface : -1;

    // Clamp selected interface to current range so indexing into interfaceNames is always safe.
    // Interfaces can disappear (e.g., USB adapter unplugged, VPN disconnected).
    if (std::cmp_greater_equal(selectedInterface, interfaceCount))
    {
        // Guard against potential overflow when converting from size_t to int.
        // While extremely unlikely (would require SIZE_MAX interfaces), be defensive.
        constexpr auto maxIntIndex = static_cast<size_t>(std::numeric_limits<int>::max());
        if ((interfaceCount == 0) || (interfaceCount > maxIntIndex))
        {
            // Fall back to "Total" mode when no interfaces or index would overflow int.
            selectedInterface = -1;
        }
        else
        {
            selectedInterface = static_cast<int>(interfaceCount) - 1;
        }

        // Update the caller's value
        if (ctx.selectedNetworkInterface != nullptr)
        {
            *ctx.selectedNetworkInterface = selectedInterface;
        }
    }

    // Interface selector
    ImGui::SetNextItemWidth(250.0F);
    // Index 0 is "Total", indices 1+ are interfaces. selectedInterface: -1 = Total, 0+ = interface index
    // Safe: selectedInterface is always >= -1, so selectedInterface + 1 is always >= 0
    const size_t comboIndex = (selectedInterface < 0) ? 0 : static_cast<size_t>(selectedInterface) + 1;
    if (ImGui::BeginCombo("##NetworkInterface", interfaceNames[comboIndex].c_str()))
    {
        for (size_t i = 0; i <= interfaceCount; ++i)
        {
            // i=0 means "Total" (selectedInterface == -1), i=1+ means interface index 0+
            const int selectionValue = static_cast<int>(i) - 1;
            const bool isSelected = (selectedInterface == selectionValue);
            if (ImGui::Selectable(interfaceNames[i].c_str(), isSelected))
            {
                selectedInterface = selectionValue;
                if (ctx.selectedNetworkInterface != nullptr)
                {
                    *ctx.selectedNetworkInterface = selectedInterface;
                }
                // Reset smoothed values when changing interface
                if (ctx.smoothedNetInitialized != nullptr)
                {
                    *ctx.smoothedNetInitialized = false;
                }
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();

    // Show link speed for selected interface (if available and not "Total")
    const bool hasValidSelection = selectedInterface >= 0 && std::cmp_less(selectedInterface, interfaceCount);
    if (hasValidSelection)
    {
        const auto& selectedIface = interfaces[static_cast<size_t>(selectedInterface)];
        if (selectedIface.linkSpeedMbps > 0)
        {
            const auto linkText = std::format("Link: {} Mbps", selectedIface.linkSpeedMbps);
            ImGui::TextColored(theme.scheme().textMuted, "%s", linkText.c_str());
        }
        else
        {
            ImGui::TextColored(theme.scheme().textMuted, "Link: Unknown");
        }
        ImGui::SameLine();
        ImGui::TextColored(selectedIface.isUp ? theme.scheme().textSuccess : theme.scheme().textError,
                           selectedIface.isUp ? "[Up]" : "[Down]");
    }

    ImGui::Spacing();

    // Get data based on selection
    double targetSent = 0.0;
    double targetRecv = 0.0;

    if (selectedInterface < 0)
    {
        // Total mode - use existing system-wide history
        targetSent = netSnap.netTxBytesPerSec;
        targetRecv = netSnap.netRxBytesPerSec;
    }
    else if (hasValidSelection)
    {
        // Specific interface - use its current rates
        const auto& selectedIface = interfaces[static_cast<size_t>(selectedInterface)];
        targetSent = selectedIface.txBytesPerSec;
        targetRecv = selectedIface.rxBytesPerSec;
    }

    const auto netTimestamps = ctx.systemModel->timestamps();
    const auto netTxHist = ctx.systemModel->netTxHistory();
    const auto netRxHist = ctx.systemModel->netRxHistory();
    const size_t aligned = std::min({netTimestamps.size(), netTxHist.size(), netRxHist.size()});

    // Get per-interface history if an interface is selected
    const bool showingInterface = selectedInterface >= 0 && hasValidSelection;
    const std::string ifaceName = showingInterface ? interfaces[static_cast<size_t>(selectedInterface)].name : "";
    const auto ifaceTxHist = showingInterface ? ctx.systemModel->netTxHistoryForInterface(ifaceName) : std::vector<float>{};
    const auto ifaceRxHist = showingInterface ? ctx.systemModel->netRxHistoryForInterface(ifaceName) : std::vector<float>{};

    // Always use default axis config even with no data
    const auto axis = aligned > 0 ? makeTimeAxisConfig(netTimestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds)
                                  : makeTimeAxisConfig({}, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

    std::vector<float> netTimes;
    std::vector<float> sentData;
    std::vector<float> recvData;
    std::vector<float> ifaceSentData;
    std::vector<float> ifaceRecvData;

    if (aligned > 0)
    {
        // Use real-time for smooth scrolling (not netTimestamps.back() which freezes between refreshes)
        netTimes = buildTimeAxis(netTimestamps, aligned, nowSeconds);
        sentData.assign(netTxHist.end() - static_cast<std::ptrdiff_t>(aligned), netTxHist.end());
        recvData.assign(netRxHist.end() - static_cast<std::ptrdiff_t>(aligned), netRxHist.end());

        // Per-interface history (if available and same length as total)
        if (showingInterface && ifaceTxHist.size() >= aligned)
        {
            ifaceSentData.assign(ifaceTxHist.end() - static_cast<std::ptrdiff_t>(aligned), ifaceTxHist.end());
        }
        if (showingInterface && ifaceRxHist.size() >= aligned)
        {
            ifaceRecvData.assign(ifaceRxHist.end() - static_cast<std::ptrdiff_t>(aligned), ifaceRxHist.end());
        }
    }

    // Update smoothed network rates
    updateSmoothedNetwork(targetSent, targetRecv, ctx.lastDeltaSeconds, ctx);

    const double smoothedSent = ctx.smoothedNetSentBytesPerSec != nullptr ? *ctx.smoothedNetSentBytesPerSec : targetSent;
    const double smoothedRecv = ctx.smoothedNetRecvBytesPerSec != nullptr ? *ctx.smoothedNetRecvBytesPerSec : targetRecv;

    // Calculate max across all data for consistent Y axis
    double netMax = std::max({sentData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(sentData)),
                              recvData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(recvData)),
                              smoothedSent,
                              smoothedRecv,
                              1.0});
    if (!ifaceSentData.empty())
    {
        netMax = std::max(netMax, static_cast<double>(*std::ranges::max_element(ifaceSentData)));
    }
    if (!ifaceRecvData.empty())
    {
        netMax = std::max(netMax, static_cast<double>(*std::ranges::max_element(ifaceRecvData)));
    }

    // Determine labels based on selection
    const std::string ifaceDisplayName = showingInterface ? interfaces[static_cast<size_t>(selectedInterface)].name : "Network";
    const std::string sentBarLabel = showingInterface ? std::format("{} Sent", ifaceDisplayName) : "Network Sent";
    const std::string recvBarLabel = showingInterface ? std::format("{} Recv", ifaceDisplayName) : "Network Received";

    const NowBar sentBar{.valueText = UI::Format::formatBytesPerSec(smoothedSent),
                         .label = sentBarLabel,
                         .value01 = std::clamp(smoothedSent / netMax, 0.0, 1.0),
                         .color = theme.scheme().chartCpu};
    const NowBar recvBar{.valueText = UI::Format::formatBytesPerSec(smoothedRecv),
                         .label = recvBarLabel,
                         .value01 = std::clamp(smoothedRecv / netMax, 0.0, 1.0),
                         .color = theme.accentColor(2)};

    // Determine plot title based on selection
    std::string plotTitle = "Total";
    if (selectedInterface >= 0 && hasValidSelection)
    {
        plotTitle = interfaces[static_cast<size_t>(selectedInterface)].name;
    }

    // Colors for interface-specific lines (lighter/dashed to distinguish from total)
    const auto ifaceSentColor = ImVec4(theme.scheme().chartCpu.x, theme.scheme().chartCpu.y, theme.scheme().chartCpu.z, 0.7F);
    const auto ifaceRecvColor = ImVec4(theme.accentColor(2).x, theme.accentColor(2).y, theme.accentColor(2).z, 0.7F);

    auto plot = [&]()
    {
        const UI::Widgets::PlotFontGuard fontGuard;
        if (ImPlot::BeginPlot("##SystemNetHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoMenus))
        {
            UI::Widgets::setupLegendDefault();
            ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
            ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisBytesPerSec);
            ImPlot::SetupAxisLimits(ImAxis_X1, axis.xMin, axis.xMax, ImPlotCond_Always);

            const int count = UI::Format::checkedCount(aligned);

            // When an interface is selected, show both total (muted) and interface (bright)
            if (showingInterface && !ifaceSentData.empty() && !ifaceRecvData.empty())
            {
                // Total lines (muted, in background)
                plotLineWithFill("Sent (Total)", netTimes.data(), sentData.data(), count, ifaceSentColor);
                plotLineWithFill("Recv (Total)", netTimes.data(), recvData.data(), count, ifaceRecvColor);

                // Interface-specific lines (bright, in foreground)
                const auto ifaceSentLabel = std::format("{} Sent", ifaceDisplayName);
                const auto ifaceRecvLabel = std::format("{} Recv", ifaceDisplayName);
                plotLineWithFill(ifaceSentLabel.c_str(), netTimes.data(), ifaceSentData.data(), count, theme.scheme().chartCpu);
                plotLineWithFill(ifaceRecvLabel.c_str(), netTimes.data(), ifaceRecvData.data(), count, theme.accentColor(2));
            }
            else
            {
                // Just total
                plotLineWithFill("Sent", netTimes.data(), sentData.data(), count, theme.scheme().chartCpu);
                plotLineWithFill("Recv", netTimes.data(), recvData.data(), count, theme.accentColor(2));
            }

            if (ImPlot::IsPlotHovered())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(netTimes, mouse.x))
                {
                    if (*idxVal < aligned)
                    {
                        ImGui::BeginTooltip();
                        const auto ageText = formatAgeSeconds(static_cast<double>(netTimes[*idxVal]));
                        ImGui::TextUnformatted(ageText.c_str());
                        ImGui::Separator();
                        if (showingInterface && !ifaceSentData.empty() && !ifaceRecvData.empty())
                        {
                            // Show both total and interface values
                            ImGui::TextColored(theme.scheme().textMuted, "Total:");
                            ImGui::TextColored(ifaceSentColor,
                                               "  Sent: %s",
                                               UI::Format::formatBytesPerSec(static_cast<double>(sentData[*idxVal])).c_str());
                            ImGui::TextColored(ifaceRecvColor,
                                               "  Recv: %s",
                                               UI::Format::formatBytesPerSec(static_cast<double>(recvData[*idxVal])).c_str());
                            ImGui::Spacing();
                            ImGui::TextColored(theme.scheme().textPrimary, "%s:", ifaceDisplayName.c_str());
                            ImGui::TextColored(theme.scheme().chartCpu,
                                               "  Sent: %s",
                                               UI::Format::formatBytesPerSec(static_cast<double>(ifaceSentData[*idxVal])).c_str());
                            ImGui::TextColored(theme.accentColor(2),
                                               "  Recv: %s",
                                               UI::Format::formatBytesPerSec(static_cast<double>(ifaceRecvData[*idxVal])).c_str());
                        }
                        else
                        {
                            ImGui::TextColored(theme.scheme().chartCpu,
                                               "Sent: %s",
                                               UI::Format::formatBytesPerSec(static_cast<double>(sentData[*idxVal])).c_str());
                            ImGui::TextColored(theme.accentColor(2),
                                               "Recv: %s",
                                               UI::Format::formatBytesPerSec(static_cast<double>(recvData[*idxVal])).c_str());
                        }
                        ImGui::EndTooltip();
                    }
                }
            }

            ImPlot::EndPlot();
        }
    };

    ImGui::TextColored(
        theme.scheme().textPrimary, ICON_FA_NETWORK_WIRED "  Network Throughput - %s (%zu samples)", plotTitle.c_str(), aligned);
    constexpr size_t NETWORK_NOW_BAR_COLUMNS = 2; // Sent, Recv
    renderHistoryWithNowBars(
        "SystemNetHistoryLayout", HISTORY_PLOT_HEIGHT_DEFAULT, plot, {sentBar, recvBar}, false, NETWORK_NOW_BAR_COLUMNS);
    ImGui::Spacing();

    // Interface status table - filtered and sorted (virtual/bluetooth hidden by default)
    const auto sortedInterfaces = NetInterfaceUtils::getSortedFilteredInterfaces(interfaces);
    if (!sortedInterfaces.empty())
    {
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_LIST "  Interface Status");
        ImGui::Spacing();

        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

        if (ImGui::BeginTable("##InterfaceTable", 6, tableFlags))
        {
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 30.0F);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, 2.5F);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_None, 0.8F);
            ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_None, 1.0F);
            ImGui::TableSetupColumn("TX Rate", ImGuiTableColumnFlags_None, 1.2F);
            ImGui::TableSetupColumn("RX Rate", ImGuiTableColumnFlags_None, 1.2F);
            ImGui::TableHeadersRow();

            for (const auto& iface : sortedInterfaces)
            {
                // Determine if this row should be dimmed (interface is down)
                const bool hasActivity = (iface.txBytesPerSec > 0.0) || (iface.rxBytesPerSec > 0.0);
                const bool shouldDim = !iface.isUp;

                ImGui::TableNextRow();

                if (shouldDim)
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5F);
                }

                // Type icon column
                ImGui::TableNextColumn();
                const char* typeIcon = ICON_FA_NETWORK_WIRED;
                ImVec4 iconColor = theme.scheme().textPrimary;

                const auto& name = iface.name;
                if (name.starts_with("lo") || name.contains("Loopback"))
                {
                    typeIcon = ICON_FA_HOUSE;
                    iconColor = theme.scheme().textMuted;
                }
                else if (name.starts_with("wl") || name.starts_with("wifi") || name.starts_with("wlan") || name.contains("Wi-Fi") ||
                         name.contains("WiFi") || name.contains("Wireless"))
                {
                    typeIcon = ICON_FA_WIFI;
                    iconColor = theme.accentColor(0);
                }
                else if (name.starts_with("eth") || name.starts_with("en") || name.contains("Ethernet"))
                {
                    typeIcon = ICON_FA_ETHERNET;
                    iconColor = theme.accentColor(1);
                }
                ImGui::TextColored(iconColor, "%s", typeIcon);

                // Name
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(iface.displayName.empty() ? iface.name.c_str() : iface.displayName.c_str());

                // Status
                ImGui::TableNextColumn();
                ImGui::TextColored(iface.isUp ? theme.scheme().textSuccess : theme.scheme().textError, iface.isUp ? "Up" : "Down");

                // Speed
                ImGui::TableNextColumn();
                if (iface.linkSpeedMbps > 0)
                {
                    if (iface.linkSpeedMbps >= 1000)
                    {
                        const auto gbps = static_cast<double>(iface.linkSpeedMbps) / 1000.0;
                        if (gbps == static_cast<int>(gbps))
                        {
                            ImGui::Text("%d Gbps", static_cast<int>(gbps));
                        }
                        else
                        {
                            ImGui::Text("%.1f Gbps", gbps);
                        }
                    }
                    else
                    {
                        const auto speedText = std::format("{} Mbps", iface.linkSpeedMbps);
                        ImGui::TextUnformatted(speedText.c_str());
                    }
                }
                else
                {
                    ImGui::TextColored(theme.scheme().textMuted, "-");
                }

                // TX Rate
                ImGui::TableNextColumn();
                if (iface.txBytesPerSec > 0.0 || hasActivity)
                {
                    ImGui::TextColored(theme.scheme().chartCpu, "%s", UI::Format::formatBytesPerSec(iface.txBytesPerSec).c_str());
                }
                else
                {
                    ImGui::TextColored(theme.scheme().textMuted, "-");
                }

                // RX Rate
                ImGui::TableNextColumn();
                if (iface.rxBytesPerSec > 0.0 || hasActivity)
                {
                    ImGui::TextColored(theme.accentColor(2), "%s", UI::Format::formatBytesPerSec(iface.rxBytesPerSec).c_str());
                }
                else
                {
                    ImGui::TextColored(theme.scheme().textMuted, "-");
                }

                if (shouldDim)
                {
                    ImGui::PopStyleVar();
                }
            }

            ImGui::EndTable();
        }
    }
}

} // namespace App::NetworkSection
