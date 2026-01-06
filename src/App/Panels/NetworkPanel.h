#pragma once

#include "App/Panel.h"
#include "Domain/SystemModel.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace App
{

/// Dedicated Network Panel providing comprehensive network monitoring.
/// Shows per-interface throughput graphs, current rates, cumulative totals, and interface status.
class NetworkPanel : public Panel
{
  public:
    NetworkPanel();
    ~NetworkPanel() override;

    NetworkPanel(const NetworkPanel&) = delete;
    NetworkPanel& operator=(const NetworkPanel&) = delete;
    NetworkPanel(NetworkPanel&&) = delete;
    NetworkPanel& operator=(NetworkPanel&&) = delete;

    /// Initialize the panel (creates SystemModel for network data).
    void onAttach() override;

    /// Cleanup.
    void onDetach() override;

    /// Update logic (refresh cadence driven by main loop).
    void onUpdate(float deltaTime) override;

    /// Set the refresh interval.
    void setSamplingInterval(std::chrono::milliseconds interval);

    /// Render the panel (with ImGui window wrapper).
    void render(bool* open) override;

    /// Render content only (for embedding).
    void renderContent();

  private:
    /// Render the interface selector dropdown.
    void renderInterfaceSelector();

    /// Render throughput graph.
    void renderThroughputGraph();

    /// Render current rates display.
    void renderCurrentRates() const;

    /// Render cumulative totals.
    void renderCumulativeTotals() const;

    /// Render interface status information.
    void renderInterfaceStatus() const;

    // Data model
    std::unique_ptr<Domain::SystemModel> m_SystemModel;

    // Sampling
    std::chrono::milliseconds m_SamplingInterval{1000};
    float m_TimeSinceLastRefresh = 0.0F;

    // History settings
    int m_MaxHistorySeconds = 60;

    // Selected interface (-1 = All/Total)
    int m_SelectedInterface = -1;

    // Cumulative totals (tracked since panel attached)
    uint64_t m_CumulativeRxBytes = 0;
    uint64_t m_CumulativeTxBytes = 0;
    double m_PrevRxBytes = 0.0;
    double m_PrevTxBytes = 0.0;
    bool m_CumulativeInitialized = false;

    // Smoothed values for display
    struct SmoothedValues
    {
        double rxBytesPerSec = 0.0;
        double txBytesPerSec = 0.0;
        bool initialized = false;
    };
    SmoothedValues m_SmoothedValues;
    static constexpr float SMOOTHING_FACTOR = 0.3F;
};

} // namespace App
