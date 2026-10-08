#pragma once

#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IServiceProbe.h"
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

} // namespace Platform
