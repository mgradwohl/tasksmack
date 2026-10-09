#pragma once

#include "Platform/IProcessActions.h"
#include "Platform/IProcessSecurity.h"

#include <string>
#include <utility>

namespace Platform
{

/// Linux IProcessSecurityReader (#1526): /proc/[pid]/status (ProcStatusSecurity::parseStatus()), with
/// the IDs named from the passwd and group databases, plus /proc/[pid]/attr/current and
/// /proc/[pid]/cgroup. Read through a handle on /proc/[pid] after its start time is checked against
/// the target's, so a reused PID reads as exited rather than showing a stranger's credentials.
///
/// status and cgroup are world-readable, so another user's process reads in full. attr/current can be
/// refused (EACCES under some LSM policies) or unsupported (EINVAL with no LSM); either leaves the label
/// empty rather than failing the read.
class LinuxProcessSecurityReader final : public IProcessSecurityReader
{
  public:
    LinuxProcessSecurityReader() = default;

    /// Test seam: read "<procRoot>/<pid>/..." instead of "/proc/<pid>/...".
    explicit LinuxProcessSecurityReader(std::string procRoot) : m_ProcRoot(std::move(procRoot))
    {}

    [[nodiscard]] bool hasSecurity() const override;
    [[nodiscard]] SecurityReadResult readSecurity(const ProcessTarget& target) override;

  private:
    std::string m_ProcRoot = "/proc";
};

} // namespace Platform
