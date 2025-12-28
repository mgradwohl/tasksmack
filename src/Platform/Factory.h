#pragma once

#include "Platform/IDiskProbe.h"
#include "Platform/IPathProvider.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ISystemProbe.h"

#include <memory>

namespace Platform
{

/// Creates the platform-appropriate IProcessProbe implementation.
[[nodiscard]] std::unique_ptr<IProcessProbe> makeProcessProbe();

/// Creates the platform-appropriate IProcessActions implementation.
[[nodiscard]] std::unique_ptr<IProcessActions> makeProcessActions();

/// Creates the platform-appropriate ISystemProbe implementation.
[[nodiscard]] std::unique_ptr<ISystemProbe> makeSystemProbe();

/// Creates the platform-appropriate IDiskProbe implementation.
[[nodiscard]] std::unique_ptr<IDiskProbe> makeDiskProbe();

/// Creates the platform-appropriate IPathProvider implementation.
[[nodiscard]] std::unique_ptr<IPathProvider> makePathProvider();

} // namespace Platform
