#pragma once

// The Services tab on Linux (#2006): systemd's services over D-Bus (SystemdBus::readServices()), as
// SystemdServices maps them. Read-only; starting, stopping and enabling them is a follow-up. The read is
// injectable, and so is the root it checks for a running systemd under, so the tests need neither.

#include "Platform/IServiceProbe.h"
#include "Platform/Linux/SystemdBus.h"

#include <filesystem>
#include <functional>
#include <memory>

namespace Platform
{

class LinuxServiceProbe final : public IServiceProbe
{
  public:
    using Reader = std::function<SystemdBus::ServicesRead()>;

    /// @param reader where the services are read from; empty for SystemdBus::readServices()
    /// @param root   where /run/systemd/system is looked for (systemd as PID 1, as sd_booted() checks)
    /// @param built  whether this build has libsystemd (SystemdBus::built() unless a test says otherwise)
    explicit LinuxServiceProbe(Reader reader = {}, const std::filesystem::path& root = "/", bool built = SystemdBus::built());

    [[nodiscard]] ServiceCapabilities capabilities() const override;
    [[nodiscard]] ServiceEnumeration enumerate() override;
    /// Forgets the cached unit files, so a start type just set shows on the next enumerate() (#2125).
    void forgetCachedConfig() override;

    /// Why there is no service list here, or empty when there is one.
    static constexpr const char* NOT_BUILT_REASON = "This build has no systemd support (it was built without libsystemd)";
    static constexpr const char* NOT_RUNNING_REASON = "systemd isn't running here, so there are no services to list";

  private:
    std::shared_ptr<SystemdBus::UnitFileCache> m_UnitFiles = std::make_shared<SystemdBus::UnitFileCache>(); ///< The default reader's
    Reader m_Reader;
    ServiceCapabilities m_Capabilities;
};

} // namespace Platform
