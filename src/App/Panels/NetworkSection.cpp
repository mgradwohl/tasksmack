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
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
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

using UI::Widgets::computeAlpha;
using UI::Widgets::formatAxisBytesPerSec;
using UI::Widgets::frameTimeAxis;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::initializeOrSmooth;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::plotSeries;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::SeriesRole;
using UI::Widgets::seriesStyle;

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

// One label per series, shared by its value-strip entry, tooltip row and NowBar (#1008).
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
        .chartDataGeneration = ctx.chartDataGeneration,
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
    if (ctx.systemPublication == nullptr)
    {
        return; // renderNetworkSection() checks first; kept so the cache below never reads a null publication
    }
    const auto& netSnap = ctx.systemPublication->snapshot;
    const auto& interfaces = netSnap.networkInterfaces;

    // Strings and lists built from the publication are kept until the next one (#1171).
    FrameCache frameLocalCache;
    FrameCache& cache = (ctx.cache != nullptr) ? *ctx.cache : frameLocalCache;
    // The size check also covers a new publication that happens to reuse the old one's address and
    // version: the selector is indexed by interface below. Every rebuild here builds first and commits
    // its keys last: a render exception is caught and the app carries on, and a cache whose keys were
    // committed before a throw would keep its stale contents for good.
    if (cache.publication != ctx.systemPublication || cache.version != ctx.systemPublication->version ||
        cache.interfaceNames.size() != interfaces.size() + 1)
    {
        // "Total" then each interface; virtual interfaces are marked as left out of the Total (#1106)
        auto names = NetInterfaceUtils::interfaceSelectorLabels(interfaces);
        cache.dropdownFontSize = -1.0F;
        cache.labelsBuilt = false;
        cache.rowsValid = false;
        cache.interfaceNames = std::move(names);
        cache.publication = ctx.systemPublication;
        cache.version = ctx.systemPublication->version;
    }

    // Build interface selector dropdown
    const std::vector<std::string>& interfaceNames = cache.interfaceNames;

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
    // The names' widths are measured once per publication and font size.
    const float comboExtraWidth = ImGui::GetFrameHeight() + (ImGui::GetStyle().FramePadding.x * 2.0F);
    // A tolerance rather than == on floats (CodeQL cpp/equality-on-floats), as CpuCoresSection's cache does.
    constexpr float FONT_SIZE_EPSILON = 1e-4F;
    if (const float fontSize = ImGui::GetFontSize(); std::abs(cache.dropdownFontSize - fontSize) > FONT_SIZE_EPSILON)
    {
        cache.dropdownFontSize = fontSize;
        cache.dropdownTextWidth = 0.0F;
        for (const auto& name : interfaceNames)
        {
            cache.dropdownTextWidth = std::max(cache.dropdownTextWidth, ImGui::CalcTextSize(name.c_str()).x);
        }
    }
    float dropdownWidth = interfaceNames.empty() ? 0.0F : cache.dropdownTextWidth + comboExtraWidth;

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
            // In bits, as the adapter is rated ("10 Gbit/s"); its byte-rate equivalent, which
            // compares with the rates, on hover (#1373). Built once per link speed, not every
            // frame (#1171).
            if (cache.linkTextMbps != selectedIface.linkSpeedMbps)
            {
                auto text = std::format("Link: {}", UI::Format::formatLinkSpeed(selectedIface.linkSpeedMbps));
                cache.linkText = std::move(text);
                cache.linkTextMbps = selectedIface.linkSpeedMbps;
            }
            ImGui::TextColored(theme.scheme().textMuted, "%s", cache.linkText.c_str());
            if (ImGui::IsItemHovered())
            {
                const std::string byteRate = UI::Format::formatLinkSpeedAsByteRate(selectedIface.linkSpeedMbps);
                ImGui::SetTooltip("Up to %s", byteRate.c_str());
            }
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
    static const std::string NO_INTERFACE;
    const std::string& ifaceName = showingInterface ? interfaces[static_cast<size_t>(selectedInterface)].name : NO_INTERFACE;
    const auto ifaceTxIt = ctx.systemPublication->perInterfaceTxHistory.find(ifaceName);
    const auto ifaceRxIt = ctx.systemPublication->perInterfaceRxHistory.find(ifaceName);
    static const std::vector<float> emptyHistory;
    const auto& ifaceTxHist =
        showingInterface && ifaceTxIt != ctx.systemPublication->perInterfaceTxHistory.end() ? ifaceTxIt->second : emptyHistory;
    const auto& ifaceRxHist =
        showingInterface && ifaceRxIt != ctx.systemPublication->perInterfaceRxHistory.end() ? ifaceRxIt->second : emptyHistory;

    // Always use default axis config even with no data
    const auto axis = aligned > 0 ? makeTimeAxisConfig(netTimestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds)
                                  : makeTimeAxisConfig({}, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

    // Views into the published history, not per-frame copies of it (#1018).
    std::span<const double> netTimes;
    std::span<const float> sentData;
    std::span<const float> recvData;
    std::span<const float> ifaceSentData;
    std::span<const float> ifaceRecvData;

    if (aligned > 0)
    {
        // Use real-time for smooth scrolling (not netTimestamps.back() which freezes between refreshes)
        netTimes = frameTimeAxis(netTimestamps, aligned, nowSeconds);
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
    // Only the samples in the window count, not the one trimming keeps left of it (#1145), plus the
    // bars' smoothed values, which can still be easing down from a peak that has just left it.
    const double netAxisUpper = UI::Widgets::easedRateAxisUpperBound(
        "##SystemNetHistory",
        UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(netTimes, axis.xMin, sentData, recvData, ifaceSentData, ifaceRecvData),
                                       {smoothedSent, smoothedRecv}),
        UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

    // Determine labels based on selection
    // Name the interface the way the picker above does (#1009).
    static const std::string NO_INTERFACE_NAME = "Network";
    const std::string& ifaceDisplayName = showingInterface ? interfaceNames[static_cast<size_t>(selectedInterface) + 1] : NO_INTERFACE_NAME;
    // One label per series, shared by its value-strip entry, tooltip row and NowBar (#1008). The bars
    // show the selected interface when there is one, else the totals. An adapter's name is the OS's
    // description, of any length, so the labels fit it to one row of the value strip (fitSeriesName();
    // both labels cut at the longer suffix's budget, so they name it alike); the picker and the plot
    // title keep it whole. The strip is as wide as the chart, so the Now column of
    // renderHistoryWithNowBars() below comes off the budget. Rebuilt only when the name changes or the
    // budget moves by half a pixel or more -- a resize or font change -- not every frame (#1171).
    constexpr std::size_t NET_BAR_COUNT = 2; // Sent and Received, as NETWORK_NOW_BAR_COLUMNS below
    constexpr float LABEL_BUDGET_REFIT_PX = 0.5F;
    // The same row width renderHistoryWithNowBars() caps its bar column against (#1300 review)
    const float labelBudget = UI::Widgets::seriesNameBudget(
        " Received", UI::Widgets::nowBarsReservedWidth(NET_BAR_COUNT, NET_BAR_COUNT, false, ImGui::GetContentRegionAvail().x));
    if (!cache.labelsBuilt || cache.labelsName != ifaceDisplayName || std::abs(cache.labelsBudget - labelBudget) >= LABEL_BUDGET_REFIT_PX)
    {
        cache.labelsBuilt = false;
        const std::string fittedName =
            UI::Widgets::fitSeriesName(ifaceDisplayName,
                                       labelBudget,
                                       [](std::string_view text) { return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x; });
        cache.interfaceSentLabel = std::format("{} Sent", fittedName);
        cache.interfaceRecvLabel = std::format("{} Received", fittedName);
        cache.unavailableTitle = std::format("Total (selected: {}, history unavailable)", ifaceDisplayName);
        cache.labelsName = ifaceDisplayName;
        cache.labelsBudget = labelBudget;
        cache.labelsBuilt = true;
    }
    const std::string& ifaceSentLabel = cache.interfaceSentLabel;
    const std::string& ifaceRecvLabel = cache.interfaceRecvLabel;
    const std::string_view sentBarLabel = showingInterface ? std::string_view{ifaceSentLabel} : std::string_view{TOTAL_SENT_LABEL};
    const std::string_view recvBarLabel = showingInterface ? std::string_view{ifaceRecvLabel} : std::string_view{TOTAL_RECV_LABEL};

    // Determine plot title based on selection
    const bool usingInterfaceHistory = showingInterface && !ifaceSentData.empty() && !ifaceRecvData.empty();

    // Colours of the machine totals drawn behind an interface's lines: muted, and drawn as thin
    // reference lines (SeriesRole::Reference), so they differ from the interface's by weight and not
    // by alpha alone, and from each other by marker shape (#1198).
    const auto ifaceSentColor = UI::withAlpha(theme.scheme().chartNetTx, 0.7F);
    const auto ifaceRecvColor = UI::withAlpha(theme.scheme().chartNetRx, 0.7F);

    const std::array netBars{
        NowBar{
            .valueText = UI::Format::formatBytesPerSec(smoothedSent),
            .label = sentBarLabel,
            .tooltipText = {},
            .value01 = UI::Widgets::normalizeToUnitInterval(smoothedSent, netAxisUpper),
            .color = theme.scheme().chartNetTx,
        },
        NowBar{
            .valueText = UI::Format::formatBytesPerSec(smoothedRecv),
            .label = recvBarLabel,
            .tooltipText = {},
            .value01 = UI::Widgets::normalizeToUnitInterval(smoothedRecv, netAxisUpper),
            .color = theme.scheme().chartNetRx,
        },
    };

    // With an interface selected the chart also draws the machine totals -- muted behind the
    // interface's lines, or alone when the interface has no history yet. They have no bar, but their
    // current values belong in the value strip like every series the chart draws (#1193): the latest
    // sample, as their tooltip rows show. The labels are views of constants and the values short
    // rates, so building these allocates nothing.
    std::array<UI::Widgets::ValueStripEntry, 2> totalEntries{};
    std::span<const UI::Widgets::ValueStripEntry> stripExtras;
    if (showingInterface)
    {
        const auto latestRate = [](std::span<const float> data)
        {
            return UI::Format::formatBytesPerSecOrNA(data.empty() ? std::numeric_limits<double>::quiet_NaN()
                                                                  : static_cast<double>(data.back()));
        };
        totalEntries = {
            UI::Widgets::ValueStripEntry{
                .label = TOTAL_SENT_BEHIND_LABEL,
                .value = latestRate(sentData),
                .color = usingInterfaceHistory ? ifaceSentColor : theme.scheme().chartNetTx,
            },
            UI::Widgets::ValueStripEntry{
                .label = TOTAL_RECV_BEHIND_LABEL,
                .value = latestRate(recvData),
                .color = usingInterfaceHistory ? ifaceRecvColor : theme.scheme().chartNetRx,
            },
        };
        stripExtras = totalEntries;
    }
    const bool interfaceHistoryUnavailable = showingInterface && !usingInterfaceHistory;
    // The totals' labels as the chart plots them: "(Total)" whenever an interface is selected -- drawn
    // behind its lines, or alone while it has no history -- so they match the strip's extras above,
    // whose swatches take the plotted series' markers by label, and the tooltip's rows (#1008).
    const char* const totalSentLabel = showingInterface ? TOTAL_SENT_BEHIND_LABEL : TOTAL_SENT_LABEL;
    const char* const totalRecvLabel = showingInterface ? TOTAL_RECV_BEHIND_LABEL : TOTAL_RECV_LABEL;

    const char* plotTitle = "Total";
    if (usingInterfaceHistory)
    {
        plotTitle = ifaceDisplayName.c_str();
    }
    else if (interfaceHistoryUnavailable)
    {
        plotTitle = cache.unavailableTitle.c_str();
    }

    // Shares the tab's height with the disk chart or grid below it (#959).
    const float plotHeight = (ctx.fill != nullptr) ? ctx.fill->plotHeight() : HISTORY_PLOT_HEIGHT_DEFAULT;
    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(
                UI::Widgets::rateHistoryConfigWithUpper("##SystemNetHistory", axis.xMin, axis.xMax, formatAxisBytesPerSec, netAxisUpper),
                plotHeight),
            ctx.chartDataGeneration));
        if (chart.active())
        {
            UI::Widgets::drawCollectingHint(aligned); // The same "no data yet" state on every chart (#1013)
            const int count = UI::Format::checkedCount(aligned);

            // When an interface is selected, show both total (muted) and interface (bright)
            if (usingInterfaceHistory)
            {
                // Total lines (muted, in background)
                plotSeries(TOTAL_SENT_BEHIND_LABEL,
                           netTimes.data(),
                           sentData.data(),
                           count,
                           ifaceSentColor,
                           std::nullopt,
                           seriesStyle(SeriesRole::Reference));
                plotSeries(TOTAL_RECV_BEHIND_LABEL,
                           netTimes.data(),
                           recvData.data(),
                           count,
                           ifaceRecvColor,
                           std::nullopt,
                           seriesStyle(SeriesRole::Reference, 1));

                // Interface-specific lines (bright, in foreground)
                plotSeries(ifaceSentLabel.c_str(),
                           netTimes.data(),
                           ifaceSentData.data(),
                           count,
                           theme.scheme().chartNetTx,
                           theme.scheme().chartNetTxFill,
                           seriesStyle(SeriesRole::Primary));
                plotSeries(ifaceRecvLabel.c_str(),
                           netTimes.data(),
                           ifaceRecvData.data(),
                           count,
                           theme.scheme().chartNetRx,
                           theme.scheme().chartNetRxFill,
                           seriesStyle(SeriesRole::Secondary, 0));
            }
            else
            {
                // Just total
                plotSeries(totalSentLabel,
                           netTimes.data(),
                           sentData.data(),
                           count,
                           theme.scheme().chartNetTx,
                           theme.scheme().chartNetTxFill,
                           seriesStyle(SeriesRole::Primary));
                plotSeries(totalRecvLabel,
                           netTimes.data(),
                           recvData.data(),
                           count,
                           theme.scheme().chartNetRx,
                           theme.scheme().chartNetRxFill,
                           seriesStyle(SeriesRole::Secondary, 0));
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
                            rows.push_back({.label = totalSentLabel, .color = theme.scheme().chartNetTx, .value = rate(sentData[*idxVal])});
                            rows.push_back({.label = totalRecvLabel, .color = theme.scheme().chartNetRx, .value = rate(recvData[*idxVal])});
                        }
                        UI::Widgets::renderHistoryTooltip(netTimes[*idxVal], rows);
                    }
                }
            }
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_NETWORK_WIRED "  Network Throughput - %s (%zu samples)", plotTitle, aligned);
    if (interfaceHistoryUnavailable)
    {
        // No spacing after it: the value strip below shares this line, right-aligned to the chart.
        ImGui::TextColored(theme.scheme().textMuted, "Per-interface history unavailable; showing total network history below.");
    }
    constexpr size_t NETWORK_NOW_BAR_COLUMNS = 2; // Sent, Recv
    // Drawn here rather than by renderHistoryWithNowBars() so it can list the totals behind an
    // interface (stripExtras) beside the bars' series; on the heading's line like every chart's.
    UI::Widgets::renderNowBarValueStrip(
        netBars,
        stripExtras,
        UI::Widgets::ValueStripLayout::Wrap,
        "SystemNetHistoryLayout",
        // The row renderHistoryWithNowBars() below lays out in, so the strip
        // reserves the same capped bar column the chart does (#1300 review)
        UI::Widgets::nowBarsReservedWidth(netBars.size(), NETWORK_NOW_BAR_COLUMNS, false, ImGui::GetContentRegionAvail().x));
    renderHistoryWithNowBars(
        "SystemNetHistoryLayout", plotHeight, plot, netBars, false, NETWORK_NOW_BAR_COLUMNS, false, UI::Widgets::NowBarValues::None);
    if (ctx.fill != nullptr)
    {
        ctx.fill->addPlot();
    }
    ImGui::Spacing();

    // Interface status table: down, virtual and Bluetooth interfaces are hidden unless "Show all" is
    // on, but a down interface that moved traffic this session stays listed (#1211).
    static const NetInterfaceUtils::InterfaceNameSet NO_TRAFFIC_SEEN;
    if (ctx.interfacesWithTraffic != nullptr)
    {
        NetInterfaceUtils::recordInterfaceTraffic(interfaces, *ctx.interfacesWithTraffic);
    }
    const auto& seenTraffic = (ctx.interfacesWithTraffic != nullptr) ? *ctx.interfacesWithTraffic : NO_TRAFFIC_SEEN;
    const bool showAllInterfaces = (ctx.showAllInterfaces != nullptr) && *ctx.showAllInterfaces;
    // The rows are copied and sorted when the publication, "Show all" or the set of interfaces seen
    // moving traffic (which only grows) changes, not every frame: 20-40 interfaces on Windows (#1171).
    if (!cache.rowsValid || cache.rowsShowAll != showAllInterfaces || cache.rowsSeenTraffic != seenTraffic.size())
    {
        cache.rowsValid = false;
        auto rows = NetInterfaceUtils::getInterfaceStatusRows(interfaces, showAllInterfaces, seenTraffic);
        const std::size_t hidden = NetInterfaceUtils::countHiddenInterfaces(interfaces, seenTraffic);
        cache.statusRows = std::move(rows);
        cache.hiddenCount = hidden;
        cache.rowsShowAll = showAllInterfaces;
        cache.rowsSeenTraffic = seenTraffic.size();
        cache.rowsValid = true;
    }
    const auto& sortedInterfaces = cache.statusRows;
    if (!interfaces.empty())
    {
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_LIST "  Interface Status");
        const std::size_t hiddenCount = cache.hiddenCount;
        if (ctx.showAllInterfaces != nullptr && (hiddenCount > 0 || *ctx.showAllInterfaces))
        {
            ImGui::SameLine();
            std::array<char, 64> showAllLabel{};
            std::format_to_n(showAllLabel.data(), showAllLabel.size() - 1, "Show all ({})##ShowAllInterfaces", hiddenCount);
            ImGui::Checkbox(showAllLabel.data(), ctx.showAllInterfaces);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Also list the %zu down, virtual and Bluetooth interfaces hidden by default", hiddenCount);
            }
        }
        ImGui::Spacing();

        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

        if (!sortedInterfaces.empty() && ImGui::BeginTable("##InterfaceTable", 6, tableFlags))
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
            ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_None, 1.2F); // The link speed, in bits (#1373)
            // Sent/Received, the words the charts and the process table use (#1203)
            ImGui::TableSetupColumn("Sent", ImGuiTableColumnFlags_None, 1.2F);
            ImGui::TableSetupColumn("Received", ImGuiTableColumnFlags_None, 1.2F);
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
                    // In bits, as the adapter is rated ("10 Gbit/s"); the byte rate it carries at
                    // most, in the Sent and Received columns' unit, on hover (#1373).
                    const std::string speedText = UI::Format::formatLinkSpeed(iface.linkSpeedMbps);
                    ImGui::TextUnformatted(speedText.c_str());
                    if (ImGui::IsItemHovered())
                    {
                        const std::string byteRate = UI::Format::formatLinkSpeedAsByteRate(iface.linkSpeedMbps);
                        ImGui::SetTooltip("Up to %s", byteRate.c_str());
                    }
                }
                else
                {
                    ImGui::TextColored(theme.scheme().textMuted, "-");
                }

                // Sent
                ImGui::TableNextColumn();
                if (iface.txBytesPerSec > 0.0 || hasActivity)
                {
                    ImGui::TextColored(theme.scheme().chartNetTx, "%s", UI::Format::formatBytesPerSec(iface.txBytesPerSec).c_str());
                }
                else
                {
                    ImGui::TextColored(theme.scheme().textMuted, "-");
                }

                // Received
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
    // closes before it renders. The network chart and the grid then split the tab between them --
    // but the grid always keeps at least its rows at their minimum height. One share alone let the
    // filling network chart (#1278) take height the grid's rows needed, and the tab scrolled with
    // eight disks in an 800x1400 region (#1370 review).
    const bool diskGrid = StorageSection::usesDiskGrid(ctx.storagePublication);
    {
        std::optional<UI::Widgets::FillPlotLayout> fill;
        if (ctx.fillState != nullptr)
        {
            // Measured at the width the grid will be drawn at: this tab's content region.
            const float gridMinimum =
                diskGrid ? StorageSection::diskGridMinimumHeight(ctx.storagePublication, ImGui::GetContentRegionAvail().x) : 0.0F;
            fill.emplace(*ctx.fillState, diskGrid ? 1U : 0U, gridMinimum);
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
