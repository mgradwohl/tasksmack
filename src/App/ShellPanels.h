#pragma once

// The shell's six panels and the wiring between them that does not touch the window (#2136): the
// crash history they share, the signed-in sessions' feed (#3377), the priority changes in flight that
// the Processes table and Process Details hold each other's priority controls by (#3467), the models
// handed from one panel to another, and the per-update hand-offs (capabilities, the selection count, Process Details' new
// samples, the Services sampler's background demand) and startup settings. ShellLayer adds the window,
// the tab bar, the status bar and input around it; the headless PGO training driver
// (src/Training/UiTrainingDriver.cpp) its own tab bar. One copy of the wiring, so training runs what
// the app runs: hand-copied, it had drifted (no service model, crash history, selection count,
// background demand or shared icon view).

#include <array>
#include <functional>
#include <memory>

namespace Core
{
class Event;
}

namespace App
{

class ConnectionsPanel;
class Panel;
class ProcessDetailsPanel;
class ProcessesPanel;
class ServicesPanel;
class StartupPanel;
class SystemInfoPanel;
class SystemMetricsPanel;
class UsersPanel;

// The panels live in ShellPanels.cpp (Impl), so this header pulls no panel header into the shell's includers: their
// translation units cost about 0.3 GB to compile, not 1.1-1.3 GB (#3717).
class ShellPanels
{
  public:
    /// The panels, with one icon cache for every tab's program icons (#1800): the Processes panel's.
    ShellPanels();
    ~ShellPanels();

    ShellPanels(const ShellPanels&) = delete;
    ShellPanels& operator=(const ShellPanels&) = delete;
    ShellPanels(ShellPanels&&) = delete;
    ShellPanels& operator=(ShellPanels&&) = delete;

    [[nodiscard]] ProcessesPanel& processes() noexcept;
    [[nodiscard]] ProcessDetailsPanel& processDetails() noexcept;
    [[nodiscard]] SystemMetricsPanel& systemMetrics() noexcept;
    [[nodiscard]] ServicesPanel& services() noexcept;
    [[nodiscard]] StartupPanel& startup() noexcept;
    [[nodiscard]] ConnectionsPanel& connections() noexcept;
    [[nodiscard]] UsersPanel& users() noexcept;
    [[nodiscard]] SystemInfoPanel& systemInfo() noexcept;

    /// In tab order: System, Processes, Process Details, Services, Startup, Connections, Users, System Information.
    [[nodiscard]] std::array<Panel*, 8> inTabOrder() noexcept;

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

    /// On each wake of the frame loop while the window is hidden and draws no frames (#3156): only
    /// the selected process's new samples into Process Details (ProcessDetailsPanel::takeSamples()),
    /// which the process model keeps for a few dozen generations. Every other hand-off follows what
    /// the user does in a frame, so it waits for the next one.
    void updateWhileHidden();

    /// Publishes the loaded refresh interval and history window through @p raise, as the first update
    /// does (#1079): the same events the Settings dialog raises, or the synthetic scenario's (#1413).
    static void raiseStartupSettings(const std::function<void(Core::Event&)>& raise);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace App
