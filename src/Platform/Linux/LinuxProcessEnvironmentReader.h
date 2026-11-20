#pragma once

#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"

#include <cstddef>

namespace Platform
{

/// The most of /proc/[pid]/environ read (#4386), as the Windows reader caps the block it reads: the process sizes
/// its own environ, and the kernel caps nothing. A longer one is cut at its last whole entry and marked truncated.
inline constexpr std::size_t ENVIRON_READ_CAP_BYTES = std::size_t{1024} * 1024;

/// Linux IProcessEnvironmentReader: /proc/[pid]/environ, read through a handle on /proc/[pid] so the
/// start-time check and the read hit the same process even if its PID is reused in between (#179).
///
/// The kernel allows the read for the process's own user, or with ptrace access to it (root, or
/// CAP_SYS_PTRACE); otherwise the open fails with EACCES and the result is PermissionDenied. The file
/// holds the environment the process was started with (or later wrote over that memory): a process's
/// later setenv() calls do not show.
class LinuxProcessEnvironmentReader final : public IProcessEnvironmentReader
{
  public:
    [[nodiscard]] bool hasEnvironment() const override;
    [[nodiscard]] EnvironmentReadResult readEnvironment(const ProcessTarget& target) override;
};

} // namespace Platform
