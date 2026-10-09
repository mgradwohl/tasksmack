#pragma once

#include "Platform/ISystemInfoProbe.h"

namespace Platform
{

/// Windows System Information facts (#1399), unprivileged: the OS from the CurrentVersion registry
/// key, the session from the computer-name, join, user-name, locale and time-zone APIs (#1512).
class WindowsSystemInfoProbe final : public ISystemInfoProbe
{
  public:
    [[nodiscard]] SystemInfoCapabilities capabilities() const override;
    [[nodiscard]] OsInfo readOs() override;
};

} // namespace Platform
