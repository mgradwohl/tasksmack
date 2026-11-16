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

#include <memory>

// Declarations only, so this header pulls no panel or platform header into its includers (#3717): each caller
// includes what it uses of the types below.
namespace Platform
{
class IDiskProbe;
class IElevatedRestart;
class IGPUProbe;
class IHttpsClient;
class IPrivilegedBroker;
class IProcessActions;
class IProcessMemoryMapReader;
class IStartAtLogin;
class ISystemInfoProbe;
class IUserSessions;
} // namespace Platform

namespace App
{

struct ProcessDetailsReaders;
struct ProcessesPanelPlatform;
struct ServicesPanelPlatform;
class SessionAccountCheck;
struct StartupPanelPlatform;
struct SystemModelProbes;
class TaskManagerReplacementController;

namespace Synthetic
{
class Scenario;
}

/// Makes, on the UI thread, what the panel platforms share and must be made there (#2731): the one
/// privileged broker (sharedPrivilegedBroker()), whose prompt owner is the main window, an SDL query.
/// ShellPanels::prepareAttach() calls it before any panel attaches; the Processes and System panels'
/// factories below then run on their sampler threads and find it made. Nothing in a synthetic run.
void preparePanelPlatforms(const Synthetic::Scenario* scenario);

/// The Processes panel's probe, actions, UDP collector, icon source and Find handles' readers: @p scenario's,
/// or the platform's.
/// Called on the Processes panel's sampler thread, from its start hook (#2731).
[[nodiscard]] ProcessesPanelPlatform makeProcessesPanelPlatform(const Synthetic::Scenario* scenario);

/// The System panel's probes, one factory per sampler (#2731): @p scenario's, or the platform's. Each is
/// called on its own sampler thread, from that sampler's start hook, so what a probe opens (the system
/// probe's PDH query, the disk probe's PDH object list, DXGI and NVML) belongs to the thread that reads it,
/// in its apartment, and a slow one (a DXGI/NVML enumeration, a PDH rescan) holds up only its own seed.
[[nodiscard]] SystemModelProbes makeSystemModelProbes(const Synthetic::Scenario* scenario);
[[nodiscard]] std::unique_ptr<Platform::IDiskProbe> makeStorageModelProbe(const Synthetic::Scenario* scenario);
[[nodiscard]] std::unique_ptr<Platform::IGPUProbe> makeGPUModelProbe(const Synthetic::Scenario* scenario);

/// Process Details' actions and readers: @p scenario's (its readers say "unsupported", so made-up PIDs
/// never reach real /proc entries), or the platform's.
[[nodiscard]] ProcessDetailsReaders makeProcessDetailsReaders(const Synthetic::Scenario* scenario);

/// Process Details' memory map reader (#3089): the platform's, or @p scenario's, which reads nothing. Making it
/// reads nothing; the Memory map section's view calls it on its worker. makeProcessDetailsReaders() hands
/// the panel one.
[[nodiscard]] std::unique_ptr<Platform::IProcessMemoryMapReader> makeProcessMemoryMapReader(const Synthetic::Scenario* scenario);

/// The Connections tab's process actions, for Close connection (#3066): the scenario's, or the platform's,
/// which an administrator can finish through the helper.
[[nodiscard]] std::unique_ptr<Platform::IProcessActions> makeConnectionsPanelActions(const Synthetic::Scenario* scenario);

/// The signed-in sessions Processes > Group: User shows (#3076, #3377): the platform's, or none in a synthetic
/// run, whose processes are made up.
[[nodiscard]] std::shared_ptr<Platform::IUserSessions> makeSignedInSessions(const Synthetic::Scenario* scenario);

/// The Services tab's probe and actions: the platform's, in every run.
[[nodiscard]] ServicesPanelPlatform makeServicesPanelPlatform(const Synthetic::Scenario* scenario);

/// The Startup tab's probe and actions: the platform's, in every run; with the shared sessionAccountCheck(), which
/// makes the per-user entries read-only in a TaskSmack elevated as another account (#3671).
[[nodiscard]] StartupPanelPlatform makeStartupPanelPlatform(const Synthetic::Scenario* scenario);

/// A System Information probe: the platform's, in every run (#3881). The System Information tab's model takes one, and
/// the System tab's sensor sampler its own (#2737), for readSensors() only, built on the sampler's first pass, off
/// the UI thread, which is safe: making it reads nothing and touches no shared state.
[[nodiscard]] std::unique_ptr<Platform::ISystemInfoProbe> makeSystemInfoProbe(const Synthetic::Scenario* scenario);

/// The HTTPS client the opt-in network features ask through, the update check (#3093) and Look up on VirusTotal
/// (#3094): the platform's, or null in a synthetic run or while a test hook is set (@p testHookActive), where neither
/// is offered. Making it opens and sends nothing.
[[nodiscard]] std::shared_ptr<Platform::IHttpsClient> makeHttpsClient(const Synthetic::Scenario* scenario, bool testHookActive);

/// The sign-in entry Settings' "Start TaskSmack when I sign in" reads and writes (#3093, part 2): the
/// platform's (null on Linux, which has none yet), or null in a synthetic run or while a test hook is set
/// (@p testHookActive), so no unattended run can ever write it. Making it reads nothing.
[[nodiscard]] std::shared_ptr<Platform::IStartAtLogin> makeStartAtLoginEntry(const Synthetic::Scenario* scenario, bool testHookActive);

/// Whose TaskSmack this is (#3671): the one check Start at sign-in and the Startup tab share, started on its worker by
/// the first call (the Settings layer's, at startup) and read once. Its holders keep it alive; the next call after
/// the last of them is gone starts another. In every run, a synthetic one too: the Startup tab reads the real
/// platform there. Call it on the UI thread.
[[nodiscard]] std::shared_ptr<SessionAccountCheck> sessionAccountCheck();

/// The probe the shared crash history reads (#1675), or null in a synthetic run: no crash history.
[[nodiscard]] std::shared_ptr<Platform::ISystemInfoProbe> makeCrashHistoryProbe(const Synthetic::Scenario* scenario);

/// The one privileged broker the panels share, for the startup notice's Administrator choice (#2728):
/// null in a synthetic run and where there is none (no helper installed, already root). Made on first
/// use; call it on the UI thread, as its prompt belongs to the main window.
[[nodiscard]] std::shared_ptr<Platform::IPrivilegedBroker> privilegedBroker(const Synthetic::Scenario* scenario);

/// Restart as administrator (#2728): the platform's (Windows', when TaskSmack isn't elevated), owned by the main
/// window; null in a synthetic run, on Linux and in an elevated TaskSmack. Call it on the UI thread, as its
/// owner window handle is an SDL query.
[[nodiscard]] std::shared_ptr<Platform::IElevatedRestart> makeElevatedRestart(const Synthetic::Scenario* scenario);

/// Replace Task Manager's controller (#3072), shared by the Settings rows and the startup notice: over the
/// platform's (Windows'), whose changes go through the shared helper; unsupported in a synthetic run and on
/// Linux. Call it on the UI thread, as privilegedBroker() is.
[[nodiscard]] std::shared_ptr<TaskManagerReplacementController> makeTaskManagerReplacementController(const Synthetic::Scenario* scenario);

namespace Detail
{
/// Test seam: @p broker in place of the installed helper's for every panel platform made after it, so a
/// test sees which ones go through the helper (#2479); null puts the installed one back.
void setSharedPrivilegedBrokerForTesting(std::shared_ptr<Platform::IPrivilegedBroker> broker);
} // namespace Detail

} // namespace App
