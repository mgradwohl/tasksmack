#pragma once

#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IProcessSecurity.h"
#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ISystemProbe.h"

#include <memory>

namespace Platform
{

/// Creates the platform-appropriate IProcessProbe implementation.
[[nodiscard]] std::unique_ptr<IProcessProbe> makeProcessProbe();

/// Creates the platform-appropriate IProcessActions implementation.
[[nodiscard]] std::unique_ptr<IProcessActions> makeProcessActions();

/// Creates the platform-appropriate IProcessEnvironmentReader implementation (#179). Windows returns
/// an UnsupportedProcessEnvironmentReader (hasEnvironment() false).
[[nodiscard]] std::unique_ptr<IProcessEnvironmentReader> makeProcessEnvironmentReader();

/// Creates the platform-appropriate IProcessConnectionsReader implementation (#799). Windows returns
/// an UnsupportedProcessConnectionsReader (hasConnections() false) until #1489.
[[nodiscard]] std::unique_ptr<IProcessConnectionsReader> makeProcessConnectionsReader();

/// Creates the platform-appropriate IProcessModulesReader implementation (#802).
[[nodiscard]] std::unique_ptr<IProcessModulesReader> makeProcessModulesReader();

/// Creates the platform-appropriate IProcessSecurityReader implementation (#1526). Windows returns an
/// UnsupportedProcessSecurityReader until its token reader lands.
[[nodiscard]] std::unique_ptr<IProcessSecurityReader> makeProcessSecurityReader();

/// Creates the platform-appropriate IStartupProbe implementation (#801). Linux returns an
/// UnsupportedStartupProbe (capabilities().canEnumerate false) until XDG autostart support lands.
[[nodiscard]] std::unique_ptr<IStartupProbe> makeStartupProbe();

/// Creates the platform-appropriate IStartupActions implementation (#801, phase 2). Linux returns an
/// UnsupportedStartupActions (every capability false) until XDG autostart support lands.
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

/// Creates the platform-appropriate IServiceProbe implementation (#800). Linux returns an
/// UnsupportedServiceProbe (capabilities().canEnumerate false) until systemd support lands.
[[nodiscard]] std::unique_ptr<IServiceProbe> makeServiceProbe();

/// Creates the platform-appropriate IServiceActions implementation (#1577). Linux returns an
/// UnsupportedServiceActions (every capability false) until systemd control lands.
[[nodiscard]] std::unique_ptr<IServiceActions> makeServiceActions();

} // namespace Platform
