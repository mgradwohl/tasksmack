#pragma once

// What a run is made of (#2137): the one place that decides, for each panel, whether it reads the
// platform or the synthetic scenario (TASKSMACK_SYNTHETIC, #1413). The panels used to decide this one
// by one, each in its own onAttach(), and not the same way; now each calls its function here when no
// factory is injected (tests inject their own), and ShellPanels takes the crash history's probe from
// here too. App stays the only layer that calls Platform::make*() and Synthetic::make*(), at attach.
//
// In a synthetic run the Processes, System and Process Details panels read the scenario. The Services,
// Startup and System Information panels still read the real platform there, as they always have; the
// crash history does not (a real crash list would match the scenario's made-up PIDs by accident).

#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/ServicesPanel.h"
#include "Panels/StartupPanel.h"
#include "Panels/SystemMetricsPanel.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/ISystemInfoProbe.h"

#include <memory>

namespace App
{

namespace Synthetic
{
class Scenario;
}

/// Makes, on the UI thread, what the panel platforms share and must be made there (#2731): the one
/// privileged broker (sharedPrivilegedBroker()), whose prompt owner is the main window, an SDL query.
/// ShellPanels::prepareAttach() calls it before any panel attaches; the Processes and System panels'
/// factories below then run on their sampler threads and find it made. Nothing in a synthetic run.
void preparePanelPlatforms(const Synthetic::Scenario* scenario);

/// The Processes panel's probe, actions, UDP collector and icon source: @p scenario's, or the platform's.
/// Called on the Processes panel's sampler thread, from its start hook (#2731).
[[nodiscard]] ProcessesPanelPlatform makeProcessesPanelPlatform(const Synthetic::Scenario* scenario);

/// The System panel's probes: @p scenario's, or the platform's. Called on the System panel's sampler
/// thread, from its start hook (#2731): PDH, DXGI and NVML are opened there, in its apartment.
[[nodiscard]] SystemMetricsProbes makeSystemMetricsProbes(const Synthetic::Scenario* scenario);

/// Process Details' actions and readers: @p scenario's (its readers say "unsupported", so made-up PIDs
/// never reach real /proc entries), or the platform's.
[[nodiscard]] ProcessDetailsReaders makeProcessDetailsReaders(const Synthetic::Scenario* scenario);

/// The Services tab's probe and actions: the platform's, in every run.
[[nodiscard]] ServicesPanelPlatform makeServicesPanelPlatform(const Synthetic::Scenario* scenario);

/// The Startup tab's probe and actions: the platform's, in every run.
[[nodiscard]] StartupPanelPlatform makeStartupPanelPlatform(const Synthetic::Scenario* scenario);

/// The System Information tab's probe: the platform's, in every run.
[[nodiscard]] std::unique_ptr<Platform::ISystemInfoProbe> makeSystemInfoPanelProbe(const Synthetic::Scenario* scenario);
/// The System tab's sensor sampler's own probe (#2737), for readSensors() only: unbrokered, and safe to
/// build off the UI thread (the sampler's first pass builds it).
[[nodiscard]] std::unique_ptr<Platform::ISystemInfoProbe> makeSensorProbe(const Synthetic::Scenario* scenario);

/// The probe the shared crash history reads (#1675), or null in a synthetic run: no crash history.
[[nodiscard]] std::shared_ptr<Platform::ISystemInfoProbe> makeCrashHistoryProbe(const Synthetic::Scenario* scenario);

/// The crash history's probe over the platform's @p probe: through @p broker, the helper System
/// Information's probe shares, so both read every user's crashes once it runs (#2416). makeCrashHistoryProbe()'s
/// seam, for tests.
[[nodiscard]] std::shared_ptr<Platform::ISystemInfoProbe> crashHistoryProbe(std::unique_ptr<Platform::ISystemInfoProbe> probe,
                                                                            std::shared_ptr<Platform::IPrivilegedBroker> broker);

namespace Detail
{
/// Test seam: @p broker in place of the installed helper's for every panel platform made after it, so a
/// test sees which ones go through the helper (#2479); null puts the installed one back.
void setSharedPrivilegedBrokerForTesting(std::shared_ptr<Platform::IPrivilegedBroker> broker);
} // namespace Detail

} // namespace App
