#pragma once

#include "Platform/IDiskProbe.h"
#include "Platform/IElevatedRestart.h"
#include "Platform/IGPUProbe.h"
#ifdef _WIN32
#include "Platform/IHandedOverFolder.h" // checkHandedOverConfigDir()
#endif
#include "Platform/IHttpsClient.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessIconSource.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/IProcessThreads.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartAtLogin.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ISystemConnections.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/ITaskManagerReplacement.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/IUserSessions.h"
#include "Platform/SessionAccount.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace Platform
{

/// Creates the platform-appropriate IProcessProbe implementation.
[[nodiscard]] std::unique_ptr<IProcessProbe> makeProcessProbe();

/// Creates the opt-in per-process UDP byte collector (#1492), stopped: Windows' ETW session on
/// Microsoft-Windows-Kernel-Network (WindowsEtwNetCollector); on Linux, an eBPF UDP counter (LinuxUdpTrafficCollector,
/// #3366) when TaskSmack runs as root (#3580), and null otherwise, since sock_diag has no UDP byte counters.
/// Shared: App starts and stops it, and the process probe reads it (IProcessProbe::setUdpTrafficCollector()).
[[nodiscard]] std::shared_ptr<IUdpTrafficCollector> makeUdpTrafficCollector();

/// Creates the platform-appropriate IProcessActions implementation.
[[nodiscard]] std::unique_ptr<IProcessActions> makeProcessActions();

/// The privileged broker (#2307, #2308): Windows only, TaskSmackHelper.exe started through UAC
/// (#2550), where it is beside TaskSmack.exe in an install only an administrator can change and
/// TaskSmack isn't already elevated; null otherwise. Linux has no helper (#3580): it runs TaskSmack as
/// Administrator (root) instead. Not started here: the first
/// IProcessActions::runAuthorized() starts it, after the UAC prompt. @p ownerWindow is the native window
/// that prompt belongs to (an HWND, so it comes up in front of TaskSmack), or null. App makes one and shares it between every
/// IProcessActions it composes (BrokeredProcessActions).
[[nodiscard]] std::shared_ptr<IPrivilegedBroker> makePrivilegedBroker(void* ownerWindow);

/// Creates the platform-appropriate IProcessIconSource (#1800): executables' icons on Windows, desktop
/// entries' PNG icons on Linux.
[[nodiscard]] std::unique_ptr<IProcessIconSource> makeProcessIconSource();

/// Creates the platform-appropriate IProcessEnvironmentReader implementation (#179): /proc/<pid>/environ on
/// Linux, the process's PEB on Windows (WindowsProcessEnvironmentReader).
[[nodiscard]] std::unique_ptr<IProcessEnvironmentReader> makeProcessEnvironmentReader();

/// Creates the platform-appropriate IProcessConnectionsReader implementation (#799; Windows #1489).
[[nodiscard]] std::unique_ptr<IProcessConnectionsReader> makeProcessConnectionsReader();

/// Creates the platform-appropriate IProcessModulesReader implementation (#802).
[[nodiscard]] std::unique_ptr<IProcessModulesReader> makeProcessModulesReader();

/// Creates the platform-appropriate IProcessThreadsReader implementation (#3069): /proc/<pid>/task on Linux,
/// SystemProcessInformation's threads on Windows (WindowsProcessThreadsReader).
[[nodiscard]] std::unique_ptr<IProcessThreadsReader> makeProcessThreadsReader();

/// Creates the platform-appropriate IProcessMemoryMapReader implementation (#3089): smaps on Linux,
/// VirtualQueryEx with GetMappedFileNameW on Windows.
[[nodiscard]] std::unique_ptr<IProcessMemoryMapReader> makeProcessMemoryMapReader();

/// Creates the platform-appropriate ISystemConnectionsReader implementation (#3066): netlink INET_DIAG on Linux, the
/// owner-PID TCP and UDP tables on Windows (WindowsSystemConnectionsReader, #3356).
[[nodiscard]] std::unique_ptr<ISystemConnectionsReader> makeSystemConnectionsReader();

/// Creates the platform-appropriate IUserSessions implementation (#3076): logind's sessions on Linux, the
/// WTS sessions on Windows (App wraps those in BrokeredUserSessions for another user's session).
[[nodiscard]] std::unique_ptr<IUserSessions> makeUserSessions();

/// Creates the platform-appropriate IProcessOpenFilesReader implementation (#183).
[[nodiscard]] std::unique_ptr<IProcessOpenFilesReader> makeProcessOpenFilesReader();

/// Creates the platform-appropriate IProcessSecurityReader implementation (#1526): /proc on Linux, the
/// process token on Windows.
[[nodiscard]] std::unique_ptr<IProcessSecurityReader> makeProcessSecurityReader();

/// Creates the platform-appropriate IStartupProbe implementation (#801): the Run keys, Startup folders and
/// packaged startup tasks on Windows; the XDG autostart entries on Linux (#2541).
[[nodiscard]] std::unique_ptr<IStartupProbe> makeStartupProbe();

/// Creates the platform-appropriate IStartupActions implementation (#801, phase 2); on Linux, a user copy
/// of the autostart file with Hidden set (#2541).
[[nodiscard]] std::unique_ptr<IStartupActions> makeStartupActions();

/// Creates the platform-appropriate ISystemInfoProbe, the System Information page's static facts (#1399).
[[nodiscard]] std::unique_ptr<ISystemInfoProbe> makeSystemInfoProbe();

/// Creates the platform-appropriate ISystemProbe implementation.
[[nodiscard]] std::unique_ptr<ISystemProbe> makeSystemProbe();

/// Creates the platform-appropriate IDiskProbe implementation.
[[nodiscard]] std::unique_ptr<IDiskProbe> makeDiskProbe();

/// Creates the platform-appropriate IPathProvider implementation.
[[nodiscard]] std::unique_ptr<IPathProvider> makePathProvider();

/// Creates the platform-appropriate IPowerProbe implementation.
[[nodiscard]] std::unique_ptr<IPowerProbe> makePowerProbe();

/// Creates the platform-appropriate IGPUProbe implementation.
[[nodiscard]] std::unique_ptr<IGPUProbe> makeGPUProbe();

/// Creates the platform-appropriate IServiceProbe implementation (#800): the Service Control Manager on
/// Windows; systemd over D-Bus on Linux (LinuxServiceProbe, #2006).
[[nodiscard]] std::unique_ptr<IServiceProbe> makeServiceProbe();

/// Creates the platform-appropriate IServiceActions implementation (#1577): the Service Control Manager
/// on Windows; systemd over D-Bus on Linux.
[[nodiscard]] std::unique_ptr<IServiceActions> makeServiceActions();

/// Replace Task Manager (#3072): taskmgr.exe's IFEO Debugger, read here and changed through @p broker (the
/// one the panels share, null where there is none), or in-process by an elevated TaskSmack, which turns it
/// on only for a copy no one but an administrator can change (#3280). Null on Linux, which has no Task
/// Manager to replace. Cheap: nothing is read, and no install is checked, until a worker calls it.
[[nodiscard]] std::shared_ptr<ITaskManagerReplacement> makeTaskManagerReplacement(const std::shared_ptr<IPrivilegedBroker>& broker);

/// Creates the HTTPS client the opt-in update check sends its one request with (#3093): WinHTTP on
/// Windows (WindowsHttpsClient), libcurl on Linux (LinuxHttpsClient, #3238). Null where libcurl is missing or
/// can't serve a worker thread (LinuxHttpsClient::usable()): the check isn't offered then, and nothing is sent.
/// Making it opens nothing; each request opens and closes its own session.
[[nodiscard]] std::shared_ptr<IHttpsClient> makeHttpsClient();

/// Creates the sign-in entry Settings' "Start TaskSmack when I sign in" reads and writes (#3093, part 2):
/// the HKCU Run key's TaskSmack value on Windows (WindowsStartAtLogin); on Linux, the per-user XDG autostart
/// entry ~/.config/autostart/tasksmack.desktop (LinuxStartAtLogin, #3254). Making it reads nothing.
[[nodiscard]] std::shared_ptr<IStartAtLogin> makeStartAtLogin();

/// Restart as administrator (#2728): TaskSmack started again elevated, through the UAC prompt, owned by
/// @p ownerWindow (an HWND, or null). Null on Linux, whose Administrator session is started as one (#3580), and
/// in a TaskSmack that is already elevated. Reads the token's elevation type once; starts nothing.
[[nodiscard]] std::shared_ptr<IElevatedRestart> makeElevatedRestart(void* ownerWindow);

/// Whether TaskSmack runs elevated as another account than the session's signed-in user (#3671), for the per-user
/// startup it then leaves alone: on Windows, TaskSmack's own token user against WTS's for the current session
/// (Windows::readSessionAccount()); either unreadable, the same account. On Linux, an Administrator session (#3580)
/// started for a user, which runs as root (SessionUser::sessionAccount(), #3682). Blocking (an account lookup): read
/// once, on a worker
/// (App::SessionAccountCheck), never on the UI thread.
[[nodiscard]] SessionAccount readSessionAccount();

#ifdef _WIN32
/// Whether TaskSmack runs elevated through UAC (TokenElevationTypeFull, #3920): its environment may then be the
/// less-privileged side's (a same-user elevation reads the user's own HKCU\Environment), so main.cpp ignores the
/// TASKSMACK_CONFIG_DIR test hook. False unelevated, and for a token of type TokenElevationTypeDefault, which has no
/// linked less-privileged token: a hosted CI runner, whose tests run elevated with one and set the hook. The built-in
/// Administrator's token is Default too, an over-the-shoulder Restart as administrator to it included, so a check
/// that must cover that restart also looks for --handoff (DllSearchPolicy.h, #3897).
[[nodiscard]] bool elevatedThroughUac();

/// The elevated copy's wait for the Standard TaskSmack that restarted it (#2728), process @p pid created at
/// @p startTicks, to exit: bounded, so a Standard copy that hangs doesn't stop it, and skipped for a PID another
/// process has taken. Another account's elevation often may not open the process (Unopenable); the instance
/// lock, tried again for the rest of HANDOFF_PATIENCE, is the hand-off then (main.cpp). @p onProgress, when set, is
/// told every HANDOFF_PROGRESS_EVERY how long it has waited (#3781). Called once, by main.cpp, before anything else.
HandoffWait waitForHandoff(std::uint32_t pid, std::uint64_t startTicks, const HandoffProgress& onProgress = {});

/// Whether the config directory an elevated restart was handed (--config-dir, #2728) may be used: a real folder
/// in a user's profile, with no junction or link on its path, owned by no administrator or system account, its
/// config.toml and lock file neither links nor hard links; then held open, and checked again through that handle.
/// The folder held is returned for main.cpp to own for the rest of the run (#3787). The error says why not
/// (Windows::ElevatedRestart::checkConfigDir(), the trust boundary in IElevatedRestart.h).
[[nodiscard]] std::expected<std::shared_ptr<IHandedOverFolder>, std::string> checkHandedOverConfigDir(const std::filesystem::path& dir);

/// This account's own config folder (FOLDERID_RoamingAppData\TaskSmack), held as a handed-over one is, for an elevated
/// session that adopted no hand-off (#4106): its environment and known folders are the less-privileged side's, so the
/// folder is anchored to the token's own profile, its owner required to be this account or Administrators, and held
/// through the same PinnedFolder path a hand-off uses. Windows only; the composition root (main.cpp) calls it.
struct OwnElevatedConfigFolder
{
    std::filesystem::path path; ///< The folder, even when it couldn't be held, for the log and for adoptHandedOver()
    std::expected<std::shared_ptr<IHandedOverFolder>, std::string> held;
};
[[nodiscard]] OwnElevatedConfigFolder holdOwnElevatedConfigFolder();

/// Brings the window of another running TaskSmack.exe to the front (#3072): what Ctrl+Shift+Esc does
/// when TaskSmack replaces Task Manager and one already runs. False when there is none to show.
bool showRunningTaskSmack();

/// Enables SeDebugPrivilege on TaskSmack's own process when it runs elevated (#2128), without which
/// even an elevated TaskSmack is refused terminate, set-information and handle-duplication access to
/// SYSTEM-service processes. Called once at startup (main.cpp), before any probe or action opens a
/// process. Logs the outcome; returns whether the privilege is now enabled. Never throws, and changes
/// nothing when not elevated. Windows only: on Linux root's capabilities already cover it.
bool enableDebugPrivilege();
#endif

} // namespace Platform
