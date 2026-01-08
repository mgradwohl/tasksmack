// =============================================================================
// DEPRECATED: This file is currently unused and contains broken code.
// It references Domain::NetworkInterfaceSnapshot which doesn't exist.
// The correct type is Domain::SystemSnapshot::InterfaceSnapshot.
//
// This file is kept for reference during the panel refactoring effort.
// See docs/panel-refactor-plan.md for the migration plan.
// TODO: Either update this file to use the correct types and wire it
// into ShellLayer, or delete it after the refactoring is complete.
// =============================================================================

#include "NetworkPanel.h"

#include "App/UserConfig.h"
#include "Domain/SystemModel.h"
#include "Platform/Factory.h"
#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <numeric>
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
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::setupLegendDefault;
using UI::Widgets::smoothTowards;
using UI::Widgets::X_AXIS_FLAGS_DEFAULT;
using UI::Widgets::Y_AXIS_FLAGS_DEFAULT;

constexpr int NOW_BAR_COLUMNS = 2;
constexpr float INTERFACE_COMBO_WIDTH = 250.0F;

// Type alias for interface snapshots from SystemSnapshot
using InterfaceSnapshot = Domain::SystemSnapshot::InterfaceSnapshot;

/// Returns indices into the interfaces vector, sorted for useful display:
/// 1. Up interfaces first
/// 2. Interfaces with activity (TX+RX > 0) first
/// 3. Higher link speed first
/// 4. Alphabetically by display name
[[nodiscard]] std::vector<std::size_t> getSortedInterfaceIndices(const std::vector<InterfaceSnapshot>& interfaces)
{
    std::vector<std::size_t> indices(interfaces.size());
    std::ranges::iota(indices, 0);

    std::ranges::sort(indices,
                      [&interfaces](std::size_t a, std::size_t b)
                      {
                          const auto& ifaceA = interfaces[a];
                          const auto& ifaceB = interfaces[b];

                          // 1. Up interfaces first
                          if (ifaceA.isUp != ifaceB.isUp)
                          {
                              return ifaceA.isUp > ifaceB.isUp;
                          }

                          // 2. Interfaces with activity first
                          const bool hasActivityA = (ifaceA.txBytesPerSec + ifaceA.rxBytesPerSec) > 0.0;
                          const bool hasActivityB = (ifaceB.txBytesPerSec + ifaceB.rxBytesPerSec) > 0.0;
                          if (hasActivityA != hasActivityB)
                          {
                              return hasActivityA > hasActivityB;
                          }

                          // 3. Higher link speed first (0 = unknown, sort last among equal status)
                          if (ifaceA.linkSpeedMbps != ifaceB.linkSpeedMbps)
                          {
                              // Both have known speed: higher first
                              if (ifaceA.linkSpeedMbps > 0 && ifaceB.linkSpeedMbps > 0)
                              {
                                  return ifaceA.linkSpeedMbps > ifaceB.linkSpeedMbps;
                              }
                              // Known speed before unknown
                              return ifaceA.linkSpeedMbps > ifaceB.linkSpeedMbps;
                          }

                          // 4. Alphabetically by display name (or name if display name is empty)
                          const auto& nameA = ifaceA.displayName.empty() ? ifaceA.name : ifaceA.displayName;
                          const auto& nameB = ifaceB.displayName.empty() ? ifaceB.name : ifaceB.displayName;
                          return nameA < nameB;
                      });

    return indices;
}

/// Check if an interface is likely a virtual/loopback interface that users rarely care about
[[nodiscard]] bool isVirtualInterface(const InterfaceSnapshot& iface)
{
    const auto& name = iface.name;

    // Common loopback names
    if (name == "lo" || name == "Loopback Pseudo-Interface 1" || name.contains("loopback") || name.contains("Loopback"))
    {
        return true;
    }

    // Docker/container interfaces (Linux)
    if (name.starts_with("docker") || name.starts_with("veth") || name.starts_with("br-"))
    {
        return true;
    }

    // VPN/tunnel interfaces (Linux)
    if (name.starts_with("tun") || name.starts_with("tap"))
    {
        return true;
    }

    // WSL interfaces (Windows)
    if (name.contains("WSL") || name.contains("vEthernet"))
    {
        return true;
    }

    // Windows virtual adapters - WAN Miniport, Microsoft virtual adapters
    if (name.starts_with("WAN Miniport") || name.starts_with("Microsoft"))
    {
        return true;
    }

    // Windows filter drivers and packet schedulers (usually duplicates of real adapters)
    if (name.contains("QoS Packet Scheduler") || name.contains("WFP") || name.contains("LightWeight Filter") ||
        name.contains("Native WiFi Filter") || name.contains("Native MAC Layer"))
    {
        return true;
    }

    // 6to4 tunnel adapter
    if (name.contains("6to4"))
    {
        return true;
    }

    // Teredo tunneling
    if (name.contains("Teredo"))
    {
        return true;
    }

    // IP-HTTPS
    if (name.contains("IP-HTTPS"))
    {
        return true;
    }

    // Kernel Debug
    if (name.contains("Kernel Debug"))
    {
        return true;
    }

    // Wi-Fi Direct virtual adapters
    if (name.contains("Wi-Fi Direct"))
    {
        return true;
    }

    return false;
}

/// Check if an interface is Bluetooth (usually not useful for throughput monitoring)
[[nodiscard]] bool isBluetoothInterface(const InterfaceSnapshot& iface)
{
    const auto& name = iface.name;
    const auto& displayName = iface.displayName;

    return name.contains("Bluetooth") || displayName.contains("Bluetooth") || name.contains("bluetooth") || name.contains("bnep");
}

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
    m_SystemModel->setMaxHistorySeconds(Domain::Numeric::toDouble(m_MaxHistorySeconds));
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
        m_SystemModel->setMaxHistorySeconds(Domain::Numeric::toDouble(m_MaxHistorySeconds));
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
            const auto rxDelta = static_cast<uint64_t>(currentRx * Domain::Numeric::toDouble(intervalSec));
            const auto txDelta = static_cast<uint64_t>(currentTx * Domain::Numeric::toDouble(intervalSec));
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
    if (!isVisible())
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
        setVisible(false);
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
    const auto& theme = UI::Theme::get();

    // Get sorted indices (Up interfaces first, then by activity, speed, name)
    const auto sortedIndices = getSortedInterfaceIndices(interfaces);

    // Build filtered interface indices list (respect m_ShowAllInterfaces filter)
    std::vector<size_t> filteredIndices;
    filteredIndices.reserve(interfaceCount);
    for (const auto idx : sortedIndices)
    {
        const auto& iface = interfaces[idx];

        // Skip down interfaces in selector if filter is off
        if (!m_ShowDownInterfaces && !iface.isUp)
        {
            continue;
        }

        // Skip virtual/bluetooth interfaces if filter is off
        if (!m_ShowAllInterfaces && (isVirtualInterface(iface) || isBluetoothInterface(iface)))
        {
            continue;
        }

        filteredIndices.push_back(idx);
    }

    // Build interface names list with type icons
    std::vector<std::string> interfaceNames;
    interfaceNames.reserve(filteredIndices.size() + 1);
    interfaceNames.emplace_back(ICON_FA_NETWORK_WIRED " All Interfaces (Total)");
    for (const auto idx : filteredIndices)
    {
        const auto& iface = interfaces[idx];
        const auto& name = iface.name;

        // Determine type icon
        const char* icon = ICON_FA_NETWORK_WIRED;
        if (name.starts_with("lo") || name.contains("Loopback"))
        {
            icon = ICON_FA_HOUSE;
        }
        else if (isBluetoothInterface(iface))
        {
            icon = ICON_FA_BLUETOOTH;
        }
        else if (isVirtualInterface(iface))
        {
            icon = ICON_FA_CLOUD;
        }
        else if (name.starts_with("wl") || name.starts_with("wifi") || name.starts_with("wlan") || name.contains("Wi-Fi") ||
                 name.contains("WiFi") || name.contains("Wireless"))
        {
            icon = ICON_FA_WIFI;
        }
        else if (name.starts_with("eth") || name.starts_with("en") || name.contains("Ethernet"))
        {
            icon = ICON_FA_ETHERNET;
        }

        const auto& displayName = iface.displayName.empty() ? iface.name : iface.displayName;
        interfaceNames.push_back(std::format("{} {}{}", icon, displayName, iface.isUp ? "" : " [Down]"));
    }

    // Validate selected interface is still valid
    bool selectionValid = false;
    if (m_SelectedInterface < 0)
    {
        selectionValid = true; // "All" is always valid
    }
    else if (std::cmp_less(m_SelectedInterface, interfaceCount))
    {
        // Check if selected interface is in filtered list
        for (const auto idx : filteredIndices)
        {
            if (std::cmp_equal(idx, m_SelectedInterface))
            {
                selectionValid = true;
                break;
            }
        }
    }

    // Reset to "All" if selection is no longer valid
    if (!selectionValid)
    {
        m_SelectedInterface = -1;
        m_SmoothedValues.initialized = false;
        m_CumulativeRxBytes = 0;
        m_CumulativeTxBytes = 0;
        m_CumulativeInitialized = false;
    }

    // Find the display position of the currently selected interface in filtered order
    std::size_t displayIndex = 0; // 0 = "All Interfaces"
    if (m_SelectedInterface >= 0)
    {
        for (std::size_t i = 0; i < filteredIndices.size(); ++i)
        {
            if (std::cmp_equal(filteredIndices[i], m_SelectedInterface))
            {
                displayIndex = i + 1; // +1 because index 0 is "All Interfaces"
                break;
            }
        }
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Interface:");
    ImGui::SameLine();

    ImGui::SetNextItemWidth(INTERFACE_COMBO_WIDTH);
    if (ImGui::BeginCombo("##InterfaceSelector", interfaceNames[displayIndex].c_str()))
    {
        for (size_t i = 0; i <= filteredIndices.size(); ++i)
        {
            // Map display index to actual interface index
            // i=0 is "All Interfaces" (-1), i>0 maps to filteredIndices[i-1]
            const int actualIndex = (i == 0) ? -1 : static_cast<int>(filteredIndices[i - 1]);
            const bool isSelected = (m_SelectedInterface == actualIndex);
            if (ImGui::Selectable(interfaceNames[i].c_str(), isSelected))
            {
                m_SelectedInterface = actualIndex;
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
    const bool hasValidSelection = m_SelectedInterface >= 0 && std::cmp_less(m_SelectedInterface, interfaceCount);
    if (hasValidSelection)
    {
        const auto& selectedIface = interfaces[static_cast<size_t>(m_SelectedInterface)];

        ImGui::SameLine();
        if (selectedIface.linkSpeedMbps > 0)
        {
            // Format speed nicely
            std::string linkText;
            if (selectedIface.linkSpeedMbps >= 1000)
            {
                const auto gbps = static_cast<double>(selectedIface.linkSpeedMbps) / 1000.0;
                if (gbps == static_cast<int>(gbps))
                {
                    linkText = std::format("Link: {} Gbps", static_cast<int>(gbps));
                }
                else
                {
                    linkText = std::format("Link: {:.1f} Gbps", gbps);
                }
            }
            else
            {
                linkText = std::format("Link: {} Mbps", selectedIface.linkSpeedMbps);
            }
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

    // Build time axis
    const double nowSeconds =
        timestamps.empty() ? std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() : timestamps.back();

    const auto axis = aligned > 0 ? makeTimeAxisConfig(timestamps, Domain::Numeric::toDouble(m_MaxHistorySeconds), 0.0)
                                  : makeTimeAxisConfig({}, Domain::Numeric::toDouble(m_MaxHistorySeconds), 0.0);

    std::vector<float> timesVec;
    std::vector<float> txData;
    std::vector<float> rxData;

    if (aligned > 0)
    {
        timesVec = buildTimeAxis(timestamps, aligned, nowSeconds);
        txData.assign(txHist.end() - static_cast<std::ptrdiff_t>(aligned), txHist.end());
        rxData.assign(rxHist.end() - static_cast<std::ptrdiff_t>(aligned), rxHist.end());
    }

    // Calculate max for Y axis
    const double netMax = std::max({txData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(txData)),
                                    rxData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(rxData)),
                                    m_SmoothedValues.txBytesPerSec,
                                    m_SmoothedValues.rxBytesPerSec,
                                    1.0});

    // Create now bars
    const NowBar txBar{.valueText = UI::Format::formatBytesPerSec(m_SmoothedValues.txBytesPerSec),
                       .label = "Sent",
                       .value01 = std::clamp(m_SmoothedValues.txBytesPerSec / netMax, 0.0, 1.0),
                       .color = theme.scheme().chartCpu};

    const NowBar rxBar{.valueText = UI::Format::formatBytesPerSec(m_SmoothedValues.rxBytesPerSec),
                       .label = "Received",
                       .value01 = std::clamp(m_SmoothedValues.rxBytesPerSec / netMax, 0.0, 1.0),
                       .color = theme.accentColor(2)};

    // Determine plot title
    std::string plotTitle = "Network Throughput";
    if (m_SelectedInterface >= 0 && std::cmp_less(m_SelectedInterface, interfaceCount))
    {
        plotTitle = std::format("Throughput: {}", interfaces[static_cast<size_t>(m_SelectedInterface)].name);
    }

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
                const auto count = Domain::Numeric::narrowOr<int>(aligned, 0);

                // Plot TX (sent)
                ImPlot::SetNextLineStyle(theme.scheme().chartCpu, 2.0F);
                plotLineWithFill("Sent",
                                 timesVec.data(),
                                 txData.data(),
                                 count,
                                 theme.scheme().chartCpu,
                                 ImVec4(theme.scheme().chartCpu.x, theme.scheme().chartCpu.y, theme.scheme().chartCpu.z, 0.3F));

                // Plot RX (received)
                const auto rxColor = theme.accentColor(2);
                ImPlot::SetNextLineStyle(rxColor, 2.0F);
                plotLineWithFill("Received", timesVec.data(), rxData.data(), count, rxColor, ImVec4(rxColor.x, rxColor.y, rxColor.z, 0.3F));
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

void NetworkPanel::renderInterfaceStatus()
{
    if (m_SystemModel == nullptr)
    {
        return;
    }

    const auto snap = m_SystemModel->snapshot();
    const auto& interfaces = snap.networkInterfaces;
    const auto& theme = UI::Theme::get();

    // Header with filter toggles
    ImGui::TextUnformatted("Interface Status");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 350.0F);
    ImGui::Checkbox("Show all interfaces", &m_ShowAllInterfaces);
    ImGui::SameLine();
    ImGui::Checkbox("Show down", &m_ShowDownInterfaces);
    ImGui::Spacing();

    if (interfaces.empty())
    {
        ImGui::TextColored(theme.scheme().textMuted, "No network interfaces detected");
        return;
    }

    // Get sorted indices (Up interfaces first, then by activity, speed, name)
    const auto sortedIndices = getSortedInterfaceIndices(interfaces);

    // Simple table showing interfaces in sorted order with filtering
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

        for (const auto idx : sortedIndices)
        {
            const auto& iface = interfaces[idx];

            // Apply filtering
            if (!m_ShowDownInterfaces && !iface.isUp)
            {
                continue;
            }

            const bool isVirtual = isVirtualInterface(iface);
            const bool isBluetooth = isBluetoothInterface(iface);

            if (!m_ShowAllInterfaces && (isVirtual || isBluetooth))
            {
                continue;
            }

            // Determine if this row should be dimmed (interface is down or virtual with no activity)
            const bool hasActivity = (iface.txBytesPerSec > 0.0) || (iface.rxBytesPerSec > 0.0);
            const bool shouldDim = !iface.isUp || (isVirtual && !hasActivity);

            ImGui::TableNextRow();

            // Apply row dimming via alpha
            if (shouldDim)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5F);
            }

            // Type icon column
            ImGui::TableNextColumn();
            const char* typeIcon = ICON_FA_NETWORK_WIRED; // Default: wired/ethernet
            ImVec4 iconColor = theme.scheme().textPrimary;

            // Determine interface type by name pattern
            const auto& name = iface.name;
            if (name.starts_with("lo") || name.contains("Loopback"))
            {
                typeIcon = ICON_FA_HOUSE;
                iconColor = theme.scheme().textMuted;
            }
            else if (isBluetooth)
            {
                typeIcon = ICON_FA_BLUETOOTH;
                iconColor = ImVec4(0.0F, 0.47F, 0.84F, 1.0F); // Bluetooth blue
            }
            else if (isVirtual)
            {
                typeIcon = ICON_FA_CLOUD;
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
                // Format speed nicely
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
                    ImGui::Text("%llu Mbps", iface.linkSpeedMbps);
                }
            }
            else
            {
                ImGui::TextColored(theme.scheme().textMuted, "-");
            }

            // TX Rate
            ImGui::TableNextColumn();
            if (iface.txBytesPerSec > 0.0)
            {
                ImGui::TextColored(theme.scheme().chartCpu, "%s", UI::Format::formatBytesPerSec(iface.txBytesPerSec).c_str());
            }
            else
            {
                ImGui::TextColored(theme.scheme().textMuted, "-");
            }

            // RX Rate
            ImGui::TableNextColumn();
            if (iface.rxBytesPerSec > 0.0)
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

} // namespace App
