#pragma once

#include "Core/Layer.h"
#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/SystemMetricsPanel.h"

namespace App
{

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
    void setupDockspace();
    void renderMenuBar();
    void renderStatusBar();

    // Panels
    ProcessesPanel m_ProcessesPanel;
    ProcessDetailsPanel m_ProcessDetailsPanel;
    SystemMetricsPanel m_SystemMetricsPanel;

    // Panel visibility
    bool m_ShowProcesses = true;
    bool m_ShowMetrics = true;
    bool m_ShowDetails = true;

    // Frame timing
    float m_FrameTime = 0.0F;
    float m_FrameTimeAccumulator = 0.0F;
    int m_FrameCount = 0;
    float m_DisplayedFps = 0.0F;
};

} // namespace App
