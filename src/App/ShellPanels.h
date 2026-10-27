#pragma once

// The shell's six panels and the wiring between them that does not touch the window (#2136): the
// crash history they share, the models handed from one panel to another, and the per-update hand-offs
// (capabilities, the selection count, Process Details' new samples, the Services sampler's background
// demand) and startup settings. ShellLayer adds the window, the tab bar, the status bar and input
// around it; the headless PGO training driver (src/Training/UiTrainingDriver.cpp) its own tab bar. One
// copy of the wiring, so training runs what the app runs: hand-copied, it had drifted (no service
// model, crash history, selection count, background demand or shared icon view).

#include "Domain/ProcessSnapshot.h"
#include "Panels/ConnectionsPanel.h"
#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/ServicesPanel.h"
#include "Panels/StartupPanel.h"
#include "Panels/SystemInfoPanel.h"
#include "Panels/SystemMetricsPanel.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace Core
{
class Event;
}

namespace App
{

class Panel;

class ShellPanels
{
  public:
    /// The panels, with one icon cache for every tab's program icons (#1800): the Processes panel's.
    ShellPanels();
    ~ShellPanels() = default;

    ShellPanels(const ShellPanels&) = delete;
    ShellPanels& operator=(const ShellPanels&) = delete;
    ShellPanels(ShellPanels&&) = delete;
    ShellPanels& operator=(ShellPanels&&) = delete;

    [[nodiscard]] ProcessesPanel& processes() noexcept
    {
        return m_Processes;
    }
    [[nodiscard]] ProcessDetailsPanel& processDetails() noexcept
    {
        return m_ProcessDetails;
    }
    [[nodiscard]] SystemMetricsPanel& systemMetrics() noexcept
    {
        return m_SystemMetrics;
    }
    [[nodiscard]] ServicesPanel& services() noexcept
    {
        return m_Services;
    }
    [[nodiscard]] StartupPanel& startup() noexcept
    {
        return m_Startup;
    }
    [[nodiscard]] ConnectionsPanel& connections() noexcept
    {
        return m_Connections;
    }
    [[nodiscard]] SystemInfoPanel& systemInfo() noexcept
    {
        return m_SystemInfo;
    }

    /// In tab order: System, Processes, Process Details, Services, Startup, Connections, System Information.
    [[nodiscard]] std::array<Panel*, 7> inTabOrder() noexcept
    {
        return {&m_SystemMetrics, &m_Processes, &m_ProcessDetails, &m_Services, &m_Startup, &m_Connections, &m_SystemInfo};
    }

    /// Before the panels attach: the recent crashes and hangs the System tab's reads fill and Process
    /// Details' Recent crashes line reads (#1675), so the System tab's model is built with them. Not in
    /// synthetic runs: their processes are made up, and a real crash list would match them by accident.
    void prepareAttach();

    /// After the panels attach: the process model shared with the System tab, the GPU model with the
    /// process model and the Processes table, and (not in synthetic runs) the Services tab's list with
    /// the process model and Process Details (#1880), then Process Details' first capabilities.
    void connectModels();

    /// Each update, after the panels' own onUpdate(): the startup Details tab (#1559), the process
    /// capabilities and accelerator (#1254, #1869), the selection count (#1926), the selected process's
    /// new samples into Process Details (#1098, #1172), and the Services sampler's background demand
    /// (#1880).
    void update(float deltaTime);

    /// Publishes the loaded refresh interval and history window through @p raise, as the first update
    /// does (#1079): the same events the Settings dialog raises, or the synthetic scenario's (#1413).
    static void raiseStartupSettings(const std::function<void(Core::Event&)>& raise);

  private:
    // The Processes panel first: its icon view outlives the Services and Startup tabs that draw with it.
    ProcessesPanel m_Processes;
    ProcessDetailsPanel m_ProcessDetails;
    SystemMetricsPanel m_SystemMetrics;
    ServicesPanel m_Services;
    StartupPanel m_Startup;
    ConnectionsPanel m_Connections; // #3066
    SystemInfoPanel m_SystemInfo;

    // The PID the process model watches for Process Details, and the scratch buffer its new samples
    // are collected into each update, reused to avoid allocating.
    std::int32_t m_WatchedPid = -1;
    std::vector<Domain::ProcessSample> m_PendingSamples;

    // GPU debug logging throttling.
    std::int32_t m_LastGpuLogPid = -1;
    bool m_LastGpuLogHasData = false;
};

} // namespace App
