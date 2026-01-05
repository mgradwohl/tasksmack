#include "NetworkPanel.h"

#include "App/UserConfig.h"
#include "Domain/SystemModel.h"
#include "Platform/Factory.h"
#include "UI/Format.h"
#include "UI/HistoryWidgets.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Numeric.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace App
{

namespace
{

using UI::Widgets::buildTimeAxis;
using UI::Widgets::computeAlpha;
using UI::Widgets::formatAxisBytesPerSec;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::PLOT_FLAGS_DEFAULT;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::setupLegendDefault;
using UI::Widgets::smoothTowards;
using UI::Widgets::X_AXIS_FLAGS_DEFAULT;
using UI::Widgets::Y_AXIS_FLAGS_DEFAULT;

constexpr int NOW_BAR_COLUMNS = 2;
constexpr float INTERFACE_COMBO_WIDTH = 250.0F;

} // namespace

NetworkPanel::NetworkPanel() : Panel("Network")
{
}

NetworkPanel::~NetworkPanel()
{
    m_SystemModel.reset();
}

void NetworkPanel::onAttach()
{
    auto& settings = UserConfig::get().settings();
    m_SamplingInterval = std::chrono::milliseconds(settings.refreshIntervalMs);
    m_MaxHistorySeconds = settings.maxHistorySeconds;

    m_SystemModel = std::make_unique<Domain::SystemModel>(Platform::makeSystemProbe(), Platform::makePowerProbe());
    m_SystemModel->setMaxHistorySeconds(UI::Numeric::toDouble(m_MaxHistorySeconds));
    m_SystemModel->refresh();

    m_TimeSinceLastRefresh = 0.0F;
    m_SelectedInterface = -1;
    m_CumulativeRxBytes = 0;
    m_CumulativeTxBytes = 0;
    m_CumulativeInitialized = false;
    m_SmoothedValues.initialized = false;
}

void NetworkPanel::onDetach()
{
    m_SystemModel.reset();
}

void NetworkPanel::onUpdate(float deltaTime)
{
    if (m_SystemModel == nullptr)
    {
        return;
    }

    m_TimeSinceLastRefresh += deltaTime;
    using SecondsF = std::chrono::duration<float>;
    const float intervalSec = std::chrono::duration_cast<SecondsF>(m_SamplingInterval).count();

    if (intervalSec > 0.0F && m_TimeSinceLastRefresh >= intervalSec)
    {
        m_SystemModel->setMaxHistorySeconds(UI::Numeric::toDouble(m_MaxHistorySeconds));
        m_SystemModel->refresh();

        // Update cumulative totals
        const auto snap = m_SystemModel->snapshot();
        double currentRx = 0.0;
        double currentTx = 0.0;

        if (m_SelectedInterface < 0)
        {
            // Total mode
            currentRx = snap.netRxBytesPerSec;
            currentTx = snap.netTxBytesPerSec;
        }
        else if (std::cmp_less(static_cast<size_t>(m_SelectedInterface), snap.networkInterfaces.size()))
        {
            const auto& iface = snap.networkInterfaces[static_cast<size_t>(m_SelectedInterface)];
            currentRx = iface.rxBytesPerSec;
            currentTx = iface.txBytesPerSec;
        }

        if (m_CumulativeInitialized)
        {
            // Add bytes transferred this interval
            const auto rxDelta = static_cast<uint64_t>(currentRx * UI::Numeric::toDouble(intervalSec));
            const auto txDelta = static_cast<uint64_t>(currentTx * UI::Numeric::toDouble(intervalSec));
            m_CumulativeRxBytes += rxDelta;
            m_CumulativeTxBytes += txDelta;
        }
        else
        {
            m_CumulativeInitialized = true;
        }
        m_PrevRxBytes = currentRx;
        m_PrevTxBytes = currentTx;

        // Update smoothed values
        const double alpha = computeAlpha(deltaTime, m_SamplingInterval);
        if (!m_SmoothedValues.initialized)
        {
            m_SmoothedValues.rxBytesPerSec = currentRx;
            m_SmoothedValues.txBytesPerSec = currentTx;
            m_SmoothedValues.initialized = true;
        }
        else
        {
            m_SmoothedValues.rxBytesPerSec = smoothTowards(m_SmoothedValues.rxBytesPerSec, currentRx, alpha);
            m_SmoothedValues.txBytesPerSec = smoothTowards(m_SmoothedValues.txBytesPerSec, currentTx, alpha);
        }

        m_TimeSinceLastRefresh = 0.0F;
    }
}

void NetworkPanel::setSamplingInterval(std::chrono::milliseconds interval)
{
    m_SamplingInterval = interval;
    m_TimeSinceLastRefresh = 0.0F;
}

void NetworkPanel::render(bool* open)
{
    if (!m_IsVisible)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);
    const std::string windowTitle = std::format("{} Network Monitor###NetworkPanel", ICON_FA_NETWORK_WIRED);

    if (ImGui::Begin(windowTitle.c_str(), open, ImGuiWindowFlags_None))
    {
        renderContent();
    }
    ImGui::End();

    if (open != nullptr && !(*open))
    {
        m_IsVisible = false;
    }
}

void NetworkPanel::renderContent()
{
    if (m_SystemModel == nullptr)
    {
        ImGui::TextUnformatted("Network data unavailable");
        return;
    }

    if (!m_SystemModel->capabilities().hasNetworkCounters)
    {
        ImGui::TextUnformatted("Network monitoring not supported on this platform");
        return;
    }

    renderInterfaceSelector();
    ImGui::Separator();
    renderThroughputGraph();
    ImGui::Separator();
    renderCurrentRates();
    ImGui::Separator();
    renderCumulativeTotals();
    ImGui::Separator();
    renderInterfaceStatus();
}

void NetworkPanel::renderInterfaceSelector()
{
    const auto snap = m_SystemModel->snapshot();
    const auto& interfaces = snap.networkInterfaces;
    const auto interfaceCount = interfaces.size();

    // Build interface names list
    std::vector<std::string> interfaceNames;
    interfaceNames.reserve(interfaceCount + 1);
    interfaceNames.emplace_back("All Interfaces (Total)");
    for (const auto& iface : interfaces)
    {
        interfaceNames.push_back(iface.displayName.empty() ? iface.name : iface.displayName);
    }

    // Clamp selected interface if interfaces disappeared
    if (std::cmp_greater_equal(m_SelectedInterface, interfaceCount))
    {
        // Guard against potential overflow when converting from size_t to int.
        constexpr auto maxIntIndex = static_cast<size_t>(std::numeric_limits<int>::max());
        if (interfaceCount == 0)
        {
            m_SelectedInterface = -1;
        }
        else if (interfaceCount > maxIntIndex)
        {
            m_SelectedInterface = std::numeric_limits<int>::max();
        }
        else
        {
            m_SelectedInterface = static_cast<int>(interfaceCount) - 1;
        }
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Interface:");
    ImGui::SameLine();

    ImGui::SetNextItemWidth(INTERFACE_COMBO_WIDTH);
    const size_t comboIndex = (m_SelectedInterface < 0) ? 0 : static_cast<size_t>(m_SelectedInterface) + 1;
    if (ImGui::BeginCombo("##InterfaceSelector", interfaceNames[comboIndex].c_str()))
    {
        for (size_t i = 0; i <= interfaceCount; ++i)
        {
            const int selectionValue = static_cast<int>(i) - 1;
            const bool isSelected = (m_SelectedInterface == selectionValue);
            if (ImGui::Selectable(interfaceNames[i].c_str(), isSelected))
            {
                m_SelectedInterface = selectionValue;
                m_SmoothedValues.initialized = false;
                // Reset cumulative totals when switching interfaces
                m_CumulativeRxBytes = 0;
                m_CumulativeTxBytes = 0;
                m_CumulativeInitialized = false;
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // Show interface info for selected interface
    const auto& theme = UI::Theme::get();
    const bool hasValidSelection = m_SelectedInterface >= 0 && std::cmp_less(m_SelectedInterface, interfaceCount);
    if (hasValidSelection)
    {
        const auto& selectedIface = interfaces[static_cast<size_t>(m_SelectedInterface)];

        ImGui::SameLine();
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
}

void NetworkPanel::renderThroughputGraph()
{
    const auto snap = m_SystemModel->snapshot();
    const auto& interfaces = snap.networkInterfaces;
    const auto interfaceCount = interfaces.size();
    const auto& theme = UI::Theme::get();

    // Get history data
    const auto timestamps = m_SystemModel->timestamps();
    const auto txHist = m_SystemModel->netTxHistory();
    const auto rxHist = m_SystemModel->netRxHistory();
    const size_t aligned = std::min({timestamps.size(), txHist.size(), rxHist.size()});

    // Get per-interface history if an interface is selected
    const bool showingInterface = m_SelectedInterface >= 0 && std::cmp_less(m_SelectedInterface, interfaceCount);
    const std::string ifaceName = showingInterface ? interfaces[static_cast<size_t>(m_SelectedInterface)].name : "";
    const auto ifaceTxHist = showingInterface ? m_SystemModel->netTxHistoryForInterface(ifaceName) : std::vector<float>{};
    const auto ifaceRxHist = showingInterface ? m_SystemModel->netRxHistoryForInterface(ifaceName) : std::vector<float>{};

    // Build time axis
    const double nowSeconds =
        timestamps.empty() ? std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() : timestamps.back();

    const auto axis = aligned > 0 ? makeTimeAxisConfig(timestamps, UI::Numeric::toDouble(m_MaxHistorySeconds), 0.0)
                                  : makeTimeAxisConfig({}, UI::Numeric::toDouble(m_MaxHistorySeconds), 0.0);

    std::vector<float> timesVec;
    std::vector<float> txData;
    std::vector<float> rxData;
    std::vector<float> ifaceTxData;
    std::vector<float> ifaceRxData;

    if (aligned > 0)
    {
        timesVec = buildTimeAxis(timestamps, aligned, nowSeconds);
        txData.assign(txHist.end() - static_cast<std::ptrdiff_t>(aligned), txHist.end());
        rxData.assign(rxHist.end() - static_cast<std::ptrdiff_t>(aligned), rxHist.end());

        // Per-interface history (if available and same length as total)
        if (showingInterface && ifaceTxHist.size() >= aligned)
        {
            ifaceTxData.assign(ifaceTxHist.end() - static_cast<std::ptrdiff_t>(aligned), ifaceTxHist.end());
        }
        if (showingInterface && ifaceRxHist.size() >= aligned)
        {
            ifaceRxData.assign(ifaceRxHist.end() - static_cast<std::ptrdiff_t>(aligned), ifaceRxHist.end());
        }
    }

    // Calculate max for Y axis (include interface data if available)
    double netMax = std::max({txData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(txData)),
                              rxData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(rxData)),
                              m_SmoothedValues.txBytesPerSec,
                              m_SmoothedValues.rxBytesPerSec,
                              1.0});
    if (!ifaceTxData.empty())
    {
        netMax = std::max(netMax, static_cast<double>(*std::ranges::max_element(ifaceTxData)));
    }
    if (!ifaceRxData.empty())
    {
        netMax = std::max(netMax, static_cast<double>(*std::ranges::max_element(ifaceRxData)));
    }

    // Determine labels based on selection
    const std::string txBarLabel = showingInterface ? std::format("{} Sent", ifaceName) : "Sent";
    const std::string rxBarLabel = showingInterface ? std::format("{} Recv", ifaceName) : "Received";

    // Create now bars
    const NowBar txBar{.valueText = UI::Format::formatBytesPerSec(m_SmoothedValues.txBytesPerSec),
                       .label = txBarLabel,
                       .value01 = std::clamp(m_SmoothedValues.txBytesPerSec / netMax, 0.0, 1.0),
                       .color = theme.scheme().chartCpu};

    const NowBar rxBar{.valueText = UI::Format::formatBytesPerSec(m_SmoothedValues.rxBytesPerSec),
                       .label = rxBarLabel,
                       .value01 = std::clamp(m_SmoothedValues.rxBytesPerSec / netMax, 0.0, 1.0),
                       .color = theme.accentColor(2)};

    // Determine plot title
    std::string plotTitle = "Network Throughput";
    if (showingInterface)
    {
        plotTitle = std::format("Throughput: {}", ifaceName);
    }

    // Colors for total lines when interface selected (muted)
    const auto totalTxColor = ImVec4(theme.scheme().chartCpu.x, theme.scheme().chartCpu.y, theme.scheme().chartCpu.z, 0.5F);
    const auto totalRxColor = ImVec4(theme.accentColor(2).x, theme.accentColor(2).y, theme.accentColor(2).z, 0.5F);

    // Render plot with now bars
    auto plot = [&]()
    {
        const UI::Widgets::PlotFontGuard fontGuard;
        if (ImPlot::BeginPlot("##NetworkThroughput", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoMenus))
        {
            setupLegendDefault();
            ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
            ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisBytesPerSec);
            ImPlot::SetupAxisLimits(ImAxis_X1, axis.xMin, axis.xMax, ImGuiCond_Always);

            if (!timesVec.empty())
            {
                const auto count = UI::Numeric::narrowOr<int>(aligned, 0);

                if (showingInterface && !ifaceTxData.empty() && !ifaceRxData.empty())
                {
                    // Show both total (muted) and interface (bright) lines
                    ImPlot::SetNextLineStyle(totalTxColor, 1.5F);
                    plotLineWithFill("Sent (Total)", timesVec.data(), txData.data(), count, totalTxColor);

                    ImPlot::SetNextLineStyle(totalRxColor, 1.5F);
                    plotLineWithFill("Recv (Total)", timesVec.data(), rxData.data(), count, totalRxColor);

                    // Interface-specific lines (bright)
                    const auto ifaceTxLabel = std::format("{} Sent", ifaceName);
                    const auto ifaceRxLabel = std::format("{} Recv", ifaceName);
                    ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                    plotLineWithFill(ifaceTxLabel.c_str(),
                                     timesVec.data(),
                                     ifaceTxData.data(),
                                     count,
                                     theme.scheme().chartCpu,
                                     ImVec4(theme.scheme().chartCpu.x, theme.scheme().chartCpu.y, theme.scheme().chartCpu.z, 0.3F));

                    const auto rxColor = theme.accentColor(2);
                    ImPlot::SetNextLineStyle(rxColor, 2.0F);
                    plotLineWithFill(ifaceRxLabel.c_str(),
                                     timesVec.data(),
                                     ifaceRxData.data(),
                                     count,
                                     rxColor,
                                     ImVec4(rxColor.x, rxColor.y, rxColor.z, 0.3F));
                }
                else
                {
                    // Just show total
                    ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                    plotLineWithFill("Sent",
                                     timesVec.data(),
                                     txData.data(),
                                     count,
                                     theme.scheme().chartCpu,
                                     ImVec4(theme.scheme().chartCpu.x, theme.scheme().chartCpu.y, theme.scheme().chartCpu.z, 0.3F));

                    const auto rxColor = theme.accentColor(2);
                    ImPlot::SetNextLineStyle(rxColor, 2.0F);
                    plotLineWithFill(
                        "Received", timesVec.data(), rxData.data(), count, rxColor, ImVec4(rxColor.x, rxColor.y, rxColor.z, 0.3F));
                }
            }

            ImPlot::EndPlot();
        }
    };

    renderHistoryWithNowBars("NetworkThroughputLayout", HISTORY_PLOT_HEIGHT_DEFAULT, plot, {txBar, rxBar}, false, NOW_BAR_COLUMNS);
}

void NetworkPanel::renderCurrentRates() const
{
    const auto& theme = UI::Theme::get();

    ImGui::TextUnformatted("Current Rates");
    ImGui::Spacing();

    ImGui::BeginGroup();
    ImGui::TextColored(theme.scheme().chartCpu, "%s", ICON_FA_ARROW_UP);
    ImGui::SameLine();
    ImGui::Text("Sent:     %s", UI::Format::formatBytesPerSec(m_SmoothedValues.txBytesPerSec).c_str());
    ImGui::EndGroup();

    ImGui::SameLine(0, 50.0F);

    ImGui::BeginGroup();
    ImGui::TextColored(theme.accentColor(2), "%s", ICON_FA_ARROW_DOWN);
    ImGui::SameLine();
    ImGui::Text("Received: %s", UI::Format::formatBytesPerSec(m_SmoothedValues.rxBytesPerSec).c_str());
    ImGui::EndGroup();
}

void NetworkPanel::renderCumulativeTotals() const
{
    const auto& theme = UI::Theme::get();

    ImGui::TextUnformatted("Session Totals (since panel opened)");
    ImGui::Spacing();

    ImGui::BeginGroup();
    ImGui::TextColored(theme.scheme().chartCpu, "%s", ICON_FA_ARROW_UP);
    ImGui::SameLine();
    ImGui::Text("Sent:     %s", UI::Format::formatBytes(static_cast<double>(m_CumulativeTxBytes)).c_str());
    ImGui::EndGroup();

    ImGui::SameLine(0, 50.0F);

    ImGui::BeginGroup();
    ImGui::TextColored(theme.accentColor(2), "%s", ICON_FA_ARROW_DOWN);
    ImGui::SameLine();
    ImGui::Text("Received: %s", UI::Format::formatBytes(static_cast<double>(m_CumulativeRxBytes)).c_str());
    ImGui::EndGroup();
}

void NetworkPanel::renderInterfaceStatus() const
{
    if (m_SystemModel == nullptr)
    {
        return;
    }

    const auto snap = m_SystemModel->snapshot();
    const auto& interfaces = snap.networkInterfaces;
    const auto& theme = UI::Theme::get();

    ImGui::TextUnformatted("Interface Status");
    ImGui::Spacing();

    if (interfaces.empty())
    {
        ImGui::TextColored(theme.scheme().textMuted, "No network interfaces detected");
        return;
    }

    // Simple table showing all interfaces
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##InterfaceTable", 5, tableFlags))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_None, 2.0F);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_None, 1.0F);
        ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_None, 1.0F);
        ImGui::TableSetupColumn("TX Rate", ImGuiTableColumnFlags_None, 1.5F);
        ImGui::TableSetupColumn("RX Rate", ImGuiTableColumnFlags_None, 1.5F);
        ImGui::TableHeadersRow();

        for (const auto& iface : interfaces)
        {
            ImGui::TableNextRow();

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
                const auto speedText = std::format("{} Mbps", iface.linkSpeedMbps);
                ImGui::TextUnformatted(speedText.c_str());
            }
            else
            {
                ImGui::TextColored(theme.scheme().textMuted, "Unknown");
            }

            // TX Rate
            ImGui::TableNextColumn();
            ImGui::TextColored(theme.scheme().chartCpu, "%s", UI::Format::formatBytesPerSec(iface.txBytesPerSec).c_str());

            // RX Rate
            ImGui::TableNextColumn();
            ImGui::TextColored(theme.accentColor(2), "%s", UI::Format::formatBytesPerSec(iface.rxBytesPerSec).c_str());
        }

        ImGui::EndTable();
    }
}

} // namespace App
