#pragma once

#include "Core/Layer.h"
#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/SystemMetricsPanel.h"

#include <cstdint>

namespace App
{

/// Main active tab in the application
enum class ActiveTab : std::uint8_t
{
    SystemOverview,
    Processes,
    ProcessDetails
};

class ShellLayer : public Core::Layer
{
  public:
    ShellLayer();
    ~ShellLayer() override = default;

    ShellLayer(const ShellLayer&) = delete;
    ShellLayer& operator=(const ShellLayer&) = delete;
    ShellLayer(ShellLayer&&) = delete;
    ShellLayer& operator=(ShellLayer&&) = delete;

    void onAttach() override;
    void onDetach() override;
    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onEvent(Core::Event& event) override;

  private:
    void renderTabBar();
    void renderStatusBar() const;

    // Panels
    ProcessesPanel m_ProcessesPanel;
    ProcessDetailsPanel m_ProcessDetailsPanel;
    SystemMetricsPanel m_SystemMetricsPanel;

    // Active tab
    ActiveTab m_ActiveTab = ActiveTab::SystemOverview;

    // FPS averaging window: accumulate frames over this many seconds before updating display
    static constexpr float FPS_AVERAGE_WINDOW_SECONDS = 0.5F;

    // Frame timing
    float m_FrameTime = 0.0F;

    // GPU debug logging throttling
    std::int32_t m_LastGpuLogPid = -1;
    bool m_LastGpuLogHasData = false;
    float m_FrameTimeAccumulator = 0.0F;
    uint32_t m_FrameCount = 0U;
    float m_DisplayedFps = 0.0F;
};

} // namespace App
