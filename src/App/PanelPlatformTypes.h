#pragma once

// What each panel is built from (#2137): the probes, actions and readers App/PanelPlatforms makes and the panels
// take. They live here, not in the panels' own headers, so PanelPlatforms.cpp, which makes them, needs no panel
// header: each of those pulls in its views and widgets, about 0.3 GB of compile memory apiece (#3717). Only the
// platform interfaces they hold are included.

#include "App/SessionAccountCheck.h"
#include "Platform/IPowerProbe.h"
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
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ISystemProbe.h"
#include "Platform/IUdpTrafficCollector.h"

#include <memory>

namespace App
{

/// The process probe and actions ProcessesPanel::onAttach() builds from (#1546).
struct ProcessesPanelPlatform
{
    std::unique_ptr<Platform::IProcessProbe> probe;
    std::unique_ptr<Platform::IProcessActions> actions;
    /// The opt-in UDP collector (#1492), shared with the probe; null where there is none (Linux, a
    /// synthetic scenario, most tests).
    // NOLINTNEXTLINE(readability-redundant-member-init) - lets the tests' designated initializers leave it out
    std::shared_ptr<Platform::IUdpTrafficCollector> udpCollector{};
    /// The icons beside the names (#1800); nullptr: every process shows the generic icon.
    std::unique_ptr<Platform::IProcessIconSource> icons;
    /// Find handles' readers (#3067), the ones Process Details uses; null where there are none (most tests).
    // NOLINTNEXTLINE(readability-redundant-member-init) - lets the tests' designated initializers leave it out
    std::unique_ptr<Platform::IProcessOpenFilesReader> openFiles{};
    // NOLINTNEXTLINE(readability-redundant-member-init) - lets the tests' designated initializers leave it out
    std::unique_ptr<Platform::IProcessModulesReader> modules{};
};

/// Process Details' actions and per-process readers (made by PanelPlatforms.h's makeProcessDetailsReaders(), #2137).
struct ProcessDetailsReaders
{
    std::unique_ptr<Platform::IProcessActions> actions;
    std::unique_ptr<Platform::IProcessEnvironmentReader> environment;
    std::unique_ptr<Platform::IProcessConnectionsReader> connections;
    std::unique_ptr<Platform::IProcessModulesReader> modules;
    std::unique_ptr<Platform::IProcessSecurityReader> security;
    std::unique_ptr<Platform::IProcessOpenFilesReader> openFiles;
    std::unique_ptr<Platform::IProcessThreadsReader> threads;     ///< #3069
    std::unique_ptr<Platform::IProcessMemoryMapReader> memoryMap; ///< #3089
};

/// The probe and actions ServicesPanel::onAttach() builds from (#1721).
struct ServicesPanelPlatform
{
    std::unique_ptr<Platform::IServiceProbe> probe;
    std::shared_ptr<Platform::IServiceActions> actions; ///< Shared, as ServiceActionsView holds it
};

/// The probe and actions StartupPanel::onAttach() builds from (#1721).
struct StartupPanelPlatform
{
    std::unique_ptr<Platform::IStartupProbe> probe;
    std::shared_ptr<Platform::IStartupActions> actions; ///< Shared, as StartupActionsView holds it
    /// Whose TaskSmack this is (#3671): in one elevated as another account the per-user entries are read-only.
    /// Null: the signed-in user's.
    std::shared_ptr<SessionAccountCheck> account;
};

/// The System model's probes (#2731): the system counters and the power state, read together on the
/// system sampler. A null power probe is no battery, as the model already allows.
struct SystemModelProbes
{
    std::unique_ptr<Platform::ISystemProbe> system;
    std::unique_ptr<Platform::IPowerProbe> power;
};

} // namespace App
