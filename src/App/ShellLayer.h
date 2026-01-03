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

  private:
    void renderTabBar();
    void renderStatusBar() const;

    // Panels
    ProcessesPanel m_ProcessesPanel;
    ProcessDetailsPanel m_ProcessDetailsPanel;
    SystemMetricsPanel m_SystemMetricsPanel;

    // Active tab
    ActiveTab m_ActiveTab = ActiveTab::SystemOverview;

    // Frame timing
    float m_FrameTime = 0.0F;
    float m_FrameTimeAccumulator = 0.0F;
    int m_FrameCount = 0;
    float m_DisplayedFps = 0.0F;
};

} // namespace App
