#pragma once

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"

#include <string>
#include <utility>

namespace Platform
{

/// Linux IProcessModulesReader (#802): /proc/[pid]/maps grouped into modules by pathname
/// (ProcMaps::parseProcMaps()), read through a handle on /proc/[pid] after its start time is checked
/// against the target's, so a reused PID reads as exited rather than showing a stranger's modules.
///
/// The kernel serves maps to the process's own user, or with ptrace read access (root, or
/// CAP_SYS_PTRACE); otherwise the open fails with EACCES and the result is PermissionDenied. Linux
/// keeps no file version for a shared object, so every version is empty.
class LinuxProcessModulesReader final : public IProcessModulesReader
{
  public:
    LinuxProcessModulesReader() = default;

    /// Test seam: read "<procRoot>/<pid>/..." instead of "/proc/<pid>/...".
    explicit LinuxProcessModulesReader(std::string procRoot) : m_ProcRoot(std::move(procRoot))
    {}

    [[nodiscard]] bool hasModules() const override;
    [[nodiscard]] ModulesReadResult readModules(const ProcessTarget& target) override;

  private:
    std::string m_ProcRoot = "/proc";
};

} // namespace Platform
