#pragma once

#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessIconSource.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/IProcessThreads.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/IUdpTrafficCollector.h"

#include <memory>

namespace Platform
{

/// Creates the platform-appropriate IProcessProbe implementation.
[[nodiscard]] std::unique_ptr<IProcessProbe> makeProcessProbe();

/// Creates the opt-in per-process UDP byte collector (#1492), stopped: Windows' ETW session on
/// Microsoft-Windows-Kernel-Network (WindowsEtwNetCollector). Null on Linux, which has no such source.
/// Shared: App starts and stops it, and the process probe reads it (IProcessProbe::setUdpTrafficCollector()).
[[nodiscard]] std::shared_ptr<IUdpTrafficCollector> makeUdpTrafficCollector();

/// Creates the platform-appropriate IProcessActions implementation.
[[nodiscard]] std::unique_ptr<IProcessActions> makeProcessActions();

/// The privileged broker (#2307, #2308): on Linux the pkexec-started helper, where the .deb or .rpm
/// installed it and TaskSmack isn't already root; on Windows TaskSmackHelper.exe started through UAC
/// (#2550), where it is beside TaskSmack.exe in an install only an administrator can change and
/// TaskSmack isn't already elevated; null otherwise. Not started here: the first
/// IProcessActions::runAuthorized() starts it, after the password or consent prompt. @p ownerWindow is
/// the native window that prompt belongs to (an HWND on Windows, so the UAC prompt comes up in front of
/// TaskSmack), or null; polkit's agent places its own. App makes one and shares it between every
/// IProcessActions it composes (BrokeredProcessActions).
[[nodiscard]] std::shared_ptr<IPrivilegedBroker> makePrivilegedBroker(void* ownerWindow);

/// Creates the platform-appropriate IProcessIconSource (#1800): executables' icons on Windows, desktop
/// entries' PNG icons on Linux.
[[nodiscard]] std::unique_ptr<IProcessIconSource> makeProcessIconSource();

/// Creates the platform-appropriate IProcessEnvironmentReader implementation (#179). Windows returns
/// an UnsupportedProcessEnvironmentReader (hasEnvironment() false).
[[nodiscard]] std::unique_ptr<IProcessEnvironmentReader> makeProcessEnvironmentReader();

/// Creates the platform-appropriate IProcessConnectionsReader implementation (#799; Windows #1489).
[[nodiscard]] std::unique_ptr<IProcessConnectionsReader> makeProcessConnectionsReader();

/// Creates the platform-appropriate IProcessModulesReader implementation (#802).
[[nodiscard]] std::unique_ptr<IProcessModulesReader> makeProcessModulesReader();

/// Creates the platform-appropriate IProcessThreadsReader implementation (#3069; Windows: unsupported
/// until its lane adds one).
[[nodiscard]] std::unique_ptr<IProcessThreadsReader> makeProcessThreadsReader();

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

#ifdef _WIN32
/// Enables SeDebugPrivilege on TaskSmack's own process when it runs elevated (#2128), without which
/// even an elevated TaskSmack is refused terminate, set-information and handle-duplication access to
/// SYSTEM-service processes. Called once at startup (main.cpp), before any probe or action opens a
/// process. Logs the outcome; returns whether the privilege is now enabled. Never throws, and changes
/// nothing when not elevated. Windows only: on Linux root's capabilities already cover it.
bool enableDebugPrivilege();
#endif

} // namespace Platform
