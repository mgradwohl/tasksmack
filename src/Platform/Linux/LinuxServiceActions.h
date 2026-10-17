#pragma once

// Service control on Linux (#2006 phase 2): IServiceActions over systemd's D-Bus Manager, with the
// choices decided on #2006 (see SystemdActions.h). Every action runs where systemd is the init system
// and this build has libsystemd; elsewhere each capability is false and the UI offers none.

#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/Linux/SystemdBus.h"

#include <filesystem>
#include <stop_token>
#include <string_view>

namespace Platform
{

class LinuxServiceActions final : public IServiceActions
{
  public:
    /// @p root: the filesystem whose /run/systemd/system says systemd is running (a fixture in tests);
    /// @p built whether this build has libsystemd; @p wait how long a start or stop waits.
    explicit LinuxServiceActions(const std::filesystem::path& root = "/", bool built = SystemdBus::built(), SystemdBus::UnitWait wait = {});

    [[nodiscard]] ServiceActionCapabilities capabilities() const override;
    [[nodiscard]] ServiceActionResult start(std::string_view name, const std::stop_token& stopToken) override;
    [[nodiscard]] ServiceActionResult stop(std::string_view name, const std::stop_token& stopToken) override;
    [[nodiscard]] ServiceActionResult restart(std::string_view name, const std::stop_token& stopToken) override;
    [[nodiscard]] ServiceActionResult setStartType(std::string_view name, ServiceStartType startType) override;
    /// The action with polkit allowed to prompt (#2304): the user's "Authenticate and retry".
    [[nodiscard]] ServiceActionResult
    runAuthorized(ServiceAction action, std::string_view name, ServiceStartType startType, const std::stop_token& stopToken) override;

  private:
    bool m_Available = false;
    SystemdBus::UnitWait m_Wait;
};

} // namespace Platform
