#include "NetworkSection.h"

#include "App/Panels/NetInterfaceUtils.h"
#include "App/Panels/StorageSection.h"
#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RateAxis.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App::NetworkSection
{

namespace
{

using UI::Widgets::buildTimeAxis;
using UI::Widgets::computeAlpha;
using UI::Widgets::formatAxisBytesPerSec;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::initializeOrSmooth;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderHistoryWithNowBars;

/// Update smoothed network values
void updateSmoothedNetwork(double targetSent, double targetRecv, float deltaTimeSeconds, RenderContext& ctx)
{
    if (ctx.smoothedNetSentBytesPerSec == nullptr || ctx.smoothedNetRecvBytesPerSec == nullptr || ctx.smoothedNetInitialized == nullptr)
    {
        return;
    }

    const double alpha = computeAlpha(deltaTimeSeconds, ctx.refreshInterval);

    const bool initialized = *ctx.smoothedNetInitialized;
    *ctx.smoothedNetSentBytesPerSec = initializeOrSmooth(*ctx.smoothedNetSentBytesPerSec, targetSent, alpha, initialized);
    *ctx.smoothedNetRecvBytesPerSec = initializeOrSmooth(*ctx.smoothedNetRecvBytesPerSec, targetRecv, alpha, initialized);
    *ctx.smoothedNetInitialized = true;
}

// One label per series, shared by its legend entry, tooltip row and NowBar (#1008).
constexpr const char* TOTAL_SENT_LABEL = "Sent";
constexpr const char* TOTAL_RECV_LABEL = "Received";
constexpr const char* TOTAL_SENT_BEHIND_LABEL = "Sent (Total)";
constexpr const char* TOTAL_RECV_BEHIND_LABEL = "Received (Total)";

} // namespace

void renderDiskIOSection(RenderContext& ctx)
{
    // Delegate to StorageSection - this wrapper maintains API compatibility
    StorageSection::RenderContext storageCtx{
        .publication = ctx.storagePublication,
        .maxHistorySeconds = ctx.maxHistorySeconds,
        .historyScrollSeconds = ctx.historyScrollSeconds,
        .lastDeltaSeconds = ctx.lastDeltaSeconds,
        .refreshInterval = ctx.refreshInterval,
        .smoothedReadBytesPerSec = ctx.smoothedDiskReadBytesPerSec,
        .smoothedWriteBytesPerSec = ctx.smoothedDiskWriteBytesPerSec,
        .smoothedInitialized = ctx.smoothedDiskInitialized,
        .smoothedPerDisk = ctx.smoothedPerDisk,
        .fill = ctx.fill,
    };
    StorageSection::renderStorageSection(storageCtx);
}

/// Network throughput chart + interface status table. Only called once ctx.systemPublication and
/// ctx.hasNetworkCounters are known good (see renderNetworkSection) -- extracted from it so that
/// function can put this section, the disk grid, and the "unavailable" fallback message in
/// whatever order it wants without the ~500 lines below needing to move or re-indent.
namespace
{
void renderNetworkChartAndTable(RenderContext& ctx, const UI::Theme& theme, double nowSeconds)
{
    const auto& netSnap = ctx.systemPublication->snapshot;
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

    // The selection is held by interface name (see resolveInterfaceSelection). selectedInterface is
    // this frame's index for it: -1 = "Total".
    const auto selection = NetInterfaceUtils::resolveInterfaceSelection(
        interfaces, ctx.selectedNetworkInterface != nullptr ? std::string_view{*ctx.selectedNetworkInterface} : std::string_view{});
    int selectedInterface = selection.index.has_value() ? UI::Format::checkedCount(*selection.index) : -1;
    if (selection.lost && ctx.selectedNetworkInterface != nullptr)
    {
        // The interface went away (adapter unplugged, VPN disconnected): show Total, and restart
        // the bars rather than letting them glide from the old interface's values.
        ctx.selectedNetworkInterface->clear();
        if (ctx.smoothedNetInitialized != nullptr)
        {
            *ctx.smoothedNetInitialized = false;
        }
    }

    // Calculate dropdown width based on longest interface name.
    // Use GetFrameHeight() for the arrow button and FramePadding.x*2 for text inset,
    // rather than a magic constant, so the width is correct at any font size/DPI.
    const float comboExtraWidth = ImGui::GetFrameHeight() + (ImGui::GetStyle().FramePadding.x * 2.0F);
    float dropdownWidth = 0.0F;
    for (const auto& name : interfaceNames)
    {
        dropdownWidth = std::max(dropdownWidth, ImGui::CalcTextSize(name.c_str()).x + comboExtraWidth);
    }

    // Never wider than the pane. Interface names are long ("Realtek Gaming USB 2.5GbE Family
    // Controller"), and a combo measured from the longest one ran under the scrollbar on a narrow
    // window, taking its drop-down arrow out of view (#966). ImGui clips the preview text, so a
    // capped combo stays fully operable.
    dropdownWidth = std::min(dropdownWidth, ImGui::GetContentRegionAvail().x);

    // Interface selector
    ImGui::SetNextItemWidth(dropdownWidth);
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
                    *ctx.selectedNetworkInterface = (i == 0) ? std::string{} : interfaces[i - 1].name;
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

    const auto& netTimestamps = ctx.systemPublication->timestamps;
    const auto& netTxHist = ctx.systemPublication->netTxHistory;
    const auto& netRxHist = ctx.systemPublication->netRxHistory;
    const size_t aligned = std::min({netTimestamps.size(), netTxHist.size(), netRxHist.size()});

    // Get per-interface history if an interface is selected
    const bool showingInterface = selectedInterface >= 0 && hasValidSelection;
    const std::string ifaceName = showingInterface ? interfaces[static_cast<size_t>(selectedInterface)].name : "";
    const auto ifaceTxIt = ctx.systemPublication->perInterfaceTxHistory.find(ifaceName);
    const auto ifaceRxIt = ctx.systemPublication->perInterfaceRxHistory.find(ifaceName);
    const std::vector<float> emptyHistory;
    const auto& ifaceTxHist =
        showingInterface && ifaceTxIt != ctx.systemPublication->perInterfaceTxHistory.end() ? ifaceTxIt->second : emptyHistory;
    const auto& ifaceRxHist =
        showingInterface && ifaceRxIt != ctx.systemPublication->perInterfaceRxHistory.end() ? ifaceRxIt->second : emptyHistory;

    // Always use default axis config even with no data
    const auto axis = aligned > 0 ? makeTimeAxisConfig(netTimestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds)
                                  : makeTimeAxisConfig({}, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

    // Views into the published history, not per-frame copies of it (#1018).
    std::vector<double> netTimes;
    std::span<const float> sentData;
    std::span<const float> recvData;
    std::span<const float> ifaceSentData;
    std::span<const float> ifaceRecvData;

    if (aligned > 0)
    {
        // Use real-time for smooth scrolling (not netTimestamps.back() which freezes between refreshes)
        netTimes = buildTimeAxis(netTimestamps, aligned, nowSeconds);
        sentData = UI::Widgets::tailAlignedSpan(netTxHist, aligned).values;
        recvData = UI::Widgets::tailAlignedSpan(netRxHist, aligned).values;

        // Per-interface history (if available and same length as total)
        if (showingInterface && ifaceTxHist.size() >= aligned)
        {
            ifaceSentData = UI::Widgets::tailAlignedSpan(ifaceTxHist, aligned).values;
        }
        if (showingInterface && ifaceRxHist.size() >= aligned)
        {
            ifaceRecvData = UI::Widgets::tailAlignedSpan(ifaceRxHist, aligned).values;
        }
    }

    // Update smoothed network rates
    updateSmoothedNetwork(targetSent, targetRecv, ctx.lastDeltaSeconds, ctx);

    const double smoothedSent = ctx.smoothedNetSentBytesPerSec != nullptr ? *ctx.smoothedNetSentBytesPerSec : targetSent;
    const double smoothedRecv = ctx.smoothedNetRecvBytesPerSec != nullptr ? *ctx.smoothedNetRecvBytesPerSec : targetRecv;

    // One upper bound for the chart's Y axis and its bars, so a bar and its line show a value at the
    // same height (#1003). It covers every series drawn on this axis, not just the totals: a
    // selected interface is plotted here too, and total vs per-interface rates are derived
    // independently, so the interface rate can exceed the total's. The interface vectors are empty
    // when none is selected, and hold NaN where the interface was absent; maxOfSeries() ignores both.
    const double netAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##SystemNetHistory",
                                             UI::Widgets::maxOfSeries(sentData, recvData, ifaceSentData, ifaceRecvData),
                                             UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

    // Determine labels based on selection
    // Name the interface the way the picker above does (#1009).
    const std::string ifaceDisplayName = showingInterface ? interfaceNames[static_cast<size_t>(selectedInterface) + 1] : "Network";
    // One label per series, shared by its legend entry, tooltip row and NowBar (#1008). The bars show
    // the selected interface when there is one, else the totals.
    const std::string ifaceSentLabel = std::format("{} Sent", ifaceDisplayName);
    const std::string ifaceRecvLabel = std::format("{} Received", ifaceDisplayName);
    const std::string sentBarLabel = showingInterface ? ifaceSentLabel : std::string(TOTAL_SENT_LABEL);
    const std::string recvBarLabel = showingInterface ? ifaceRecvLabel : std::string(TOTAL_RECV_LABEL);

    const NowBar sentBar{.valueText = UI::Format::formatBytesPerSec(smoothedSent),
                         .label = sentBarLabel,
                         .tooltipText = {},
                         .value01 = UI::Widgets::normalizeToUnitInterval(smoothedSent, netAxisUpper),
                         .color = theme.scheme().chartNetTx};
    const NowBar recvBar{.valueText = UI::Format::formatBytesPerSec(smoothedRecv),
                         .label = recvBarLabel,
                         .tooltipText = {},
                         .value01 = UI::Widgets::normalizeToUnitInterval(smoothedRecv, netAxisUpper),
                         .color = theme.scheme().chartNetRx};

    // Determine plot title based on selection
    const bool usingInterfaceHistory = showingInterface && !ifaceSentData.empty() && !ifaceRecvData.empty();
    const bool interfaceHistoryUnavailable = showingInterface && !usingInterfaceHistory;

    std::string plotTitle = "Total";
    if (usingInterfaceHistory)
    {
        plotTitle = ifaceDisplayName;
    }
    else if (interfaceHistoryUnavailable)
    {
        plotTitle = std::format("Total (selected: {}, history unavailable)", ifaceDisplayName);
    }

    // Colors for interface-specific lines (lighter/dashed to distinguish from total)
    const auto ifaceSentColor = UI::withAlpha(theme.scheme().chartNetTx, 0.7F);
    const auto ifaceRecvColor = UI::withAlpha(theme.scheme().chartNetRx, 0.7F);

    // Shares the tab's height with the disk chart or grid below it (#959).
    const float plotHeight = (ctx.fill != nullptr) ? ctx.fill->plotHeight() : HISTORY_PLOT_HEIGHT_DEFAULT;
    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withHeight(
            UI::Widgets::rateHistoryConfigWithUpper("##SystemNetHistory", axis.xMin, axis.xMax, formatAxisBytesPerSec, netAxisUpper),
            plotHeight));
        if (chart.active())
        {
            UI::Widgets::drawCollectingHint(aligned); // The same "no data yet" state on every chart (#1013)
            const int count = UI::Format::checkedCount(aligned);

            // When an interface is selected, show both total (muted) and interface (bright)
            if (usingInterfaceHistory)
            {
                // Total lines (muted, in background)
                plotLineWithFill(TOTAL_SENT_BEHIND_LABEL,
                                 netTimes.data(),
                                 sentData.data(),
                                 count,
                                 ifaceSentColor,
                                 std::nullopt,
                                 2.0F,
                                 false, // line only: the interface fills in front are the series
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                plotLineWithFill(TOTAL_RECV_BEHIND_LABEL,
                                 netTimes.data(),
                                 recvData.data(),
                                 count,
                                 ifaceRecvColor,
                                 std::nullopt,
                                 2.0F,
                                 false, // line only: the interface fills in front are the series
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);

                // Interface-specific lines (bright, in foreground)
                plotLineWithFill(ifaceSentLabel.c_str(),
                                 netTimes.data(),
                                 ifaceSentData.data(),
                                 count,
                                 theme.scheme().chartNetTx,
                                 theme.scheme().chartNetTxFill,
                                 2.0F,
                                 true,
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                plotLineWithFill(ifaceRecvLabel.c_str(),
                                 netTimes.data(),
                                 ifaceRecvData.data(),
                                 count,
                                 theme.scheme().chartNetRx,
                                 theme.scheme().chartNetRxFill,
                                 2.0F,
                                 true,
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
            }
            else
            {
                // Just total
                plotLineWithFill(TOTAL_SENT_LABEL,
                                 netTimes.data(),
                                 sentData.data(),
                                 count,
                                 theme.scheme().chartNetTx,
                                 theme.scheme().chartNetTxFill,
                                 2.0F,
                                 true,
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                plotLineWithFill(TOTAL_RECV_LABEL,
                                 netTimes.data(),
                                 recvData.data(),
                                 count,
                                 theme.scheme().chartNetRx,
                                 theme.scheme().chartNetRxFill,
                                 2.0F,
                                 true,
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
            }

            if (ImPlot::IsPlotHovered())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(netTimes, mouse.x))
                {
                    if (*idxVal < aligned)
                    {
                        const auto rate = [](float value)
                        {
                            return UI::Format::formatBytesPerSecOrNA(static_cast<double>(value));
                        };
                        std::vector<UI::Widgets::TooltipRow> rows;
                        if (usingInterfaceHistory)
                        {
                            rows.push_back(
                                {.label = ifaceSentLabel, .color = theme.scheme().chartNetTx, .value = rate(ifaceSentData[*idxVal])});
                            rows.push_back(
                                {.label = ifaceRecvLabel, .color = theme.scheme().chartNetRx, .value = rate(ifaceRecvData[*idxVal])});
                            rows.push_back({.label = TOTAL_SENT_BEHIND_LABEL, .color = ifaceSentColor, .value = rate(sentData[*idxVal])});
                            rows.push_back({.label = TOTAL_RECV_BEHIND_LABEL, .color = ifaceRecvColor, .value = rate(recvData[*idxVal])});
                        }
                        else
                        {
                            rows.push_back(
                                {.label = TOTAL_SENT_LABEL, .color = theme.scheme().chartNetTx, .value = rate(sentData[*idxVal])});
                            rows.push_back(
                                {.label = TOTAL_RECV_LABEL, .color = theme.scheme().chartNetRx, .value = rate(recvData[*idxVal])});
                        }
                        UI::Widgets::renderHistoryTooltip(netTimes[*idxVal], rows);
                    }
                }
            }
        }
    };

    ImGui::TextColored(
        theme.scheme().textPrimary, ICON_FA_NETWORK_WIRED "  Network Throughput - %s (%zu samples)", plotTitle.c_str(), aligned);
    if (interfaceHistoryUnavailable)
    {
        ImGui::TextColored(theme.scheme().textMuted, "Per-interface history unavailable; showing total network history below.");
        ImGui::Spacing();
    }
    constexpr size_t NETWORK_NOW_BAR_COLUMNS = 2; // Sent, Recv
    renderHistoryWithNowBars("SystemNetHistoryLayout", plotHeight, plot, {sentBar, recvBar}, false, NETWORK_NOW_BAR_COLUMNS);
    if (ctx.fill != nullptr)
    {
        ctx.fill->addPlot();
    }
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
            // Wide enough for its header and for any of the icons it can hold, at the current font.
            // It was a fixed 30px on a non-resizable table: the header was cut to "T..." from Extra
            // Large up on a scaled display, and to "..." at Even Huger (#966).
            constexpr auto TYPE_COLUMN_CONTENTS =
                std::to_array<const char*>({"Type", ICON_FA_NETWORK_WIRED, ICON_FA_HOUSE, ICON_FA_WIFI, ICON_FA_ETHERNET});
            float typeColumnWidth = 0.0F;
            for (const char* text : TYPE_COLUMN_CONTENTS)
            {
                typeColumnWidth = std::max(typeColumnWidth, ImGui::CalcTextSize(text).x);
            }
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, typeColumnWidth);
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
                        if ((iface.linkSpeedMbps % 1000) == 0)
                        {
                            // safe: PRIu64 handles uint64_t without narrowing
                            ImGui::Text("%" PRIu64 " Gbps", iface.linkSpeedMbps / 1000);
                        }
                        else
                        {
                            ImGui::Text("%.1f Gbps", static_cast<double>(iface.linkSpeedMbps) / 1000.0);
                        }
                    }
                    else
                    {
                        ImGui::Text("%" PRIu64 " Mbps", iface.linkSpeedMbps);
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
                    ImGui::TextColored(theme.scheme().chartNetTx, "%s", UI::Format::formatBytesPerSec(iface.txBytesPerSec).c_str());
                }
                else
                {
                    ImGui::TextColored(theme.scheme().textMuted, "-");
                }

                // RX Rate
                ImGui::TableNextColumn();
                if (iface.rxBytesPerSec > 0.0 || hasActivity)
                {
                    ImGui::TextColored(theme.scheme().chartNetRx, "%s", UI::Format::formatBytesPerSec(iface.rxBytesPerSec).c_str());
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
} // namespace

void renderNetworkSection(RenderContext& ctx)
{
    const auto& theme = UI::Theme::get();
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)

    // Network content first, at its own natural (non-stretching) height; the disk grid renders
    // after it and fills whatever's left via ImGui::GetContentRegionAvail() (see
    // StorageSection::renderStorageSection) -- reversed from the disk-then-network order this
    // tab used before #823's grid work, which let the disk grid's now-space-filling behavior
    // greedily consume the whole tab and push the network chart/table below it off-screen.
    //
    // The charts share the tab's height like every other tab's (#959). With one disk that is just
    // the network chart and the disk chart. With several, the per-disk grid still takes whatever
    // is left, so it is not measured: it is reserved one share of the height, and the fill scope
    // closes before it renders. The network chart and the grid then split the tab between them.
    const bool diskGrid = StorageSection::usesDiskGrid(ctx.storagePublication);
    {
        std::optional<UI::Widgets::FillPlotLayout> fill;
        if (ctx.fillState != nullptr)
        {
            fill.emplace(*ctx.fillState, diskGrid ? 1U : 0U);
            ctx.fill = &*fill;
        }

        if (ctx.systemPublication == nullptr || !ctx.hasNetworkCounters)
        {
            ImGui::TextUnformatted("Network monitoring not available on this platform.");
        }
        else
        {
            renderNetworkChartAndTable(ctx, theme, nowSeconds);
        }

        ImGui::Separator();
        ImGui::Spacing();
        if (!diskGrid)
        {
            renderDiskIOSection(ctx);
        }
        ctx.fill = nullptr;
    }
    if (diskGrid)
    {
        renderDiskIOSection(ctx);
    }
}

} // namespace App::NetworkSection
