#pragma once

#include "Platform/IProcessActions.h"
#include "Platform/IProcessOpenFiles.h"

#include <string>
#include <utility>

namespace Platform
{

/// Linux IProcessOpenFilesReader (#183): each entry of /proc/[pid]/fd read with readlinkat() and typed
/// by ProcFd::classifyFdLink(), with its open flags from /proc/[pid]/fdinfo/<n>. Never spawns lsof and
/// never stat()s a target. Read through a handle on /proc/[pid] after its start time is checked against
/// the target's, so a reused PID reads as exited.
///
/// The kernel shows another process's descriptors only to its own user, or with ptrace read access
/// (root, or CAP_SYS_PTRACE); otherwise opening fd/ or reading its links fails with EACCES and the
/// result is PermissionDenied.
class LinuxProcessOpenFilesReader final : public IProcessOpenFilesReader
{
  public:
    LinuxProcessOpenFilesReader() = default;

    /// Test seam: read "<procRoot>/<pid>/..." instead of "/proc/<pid>/...".
    explicit LinuxProcessOpenFilesReader(std::string procRoot) : m_ProcRoot(std::move(procRoot))
    {}

    [[nodiscard]] bool hasOpenFiles() const override;
    [[nodiscard]] OpenFilesReadResult readOpenFiles(const ProcessTarget& target) override;

  private:
    std::string m_ProcRoot = "/proc";
};

} // namespace Platform
