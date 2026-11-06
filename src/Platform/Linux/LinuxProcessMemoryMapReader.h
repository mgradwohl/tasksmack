#pragma once

#include "Platform/IProcessActions.h"
#include "Platform/IProcessMemoryMap.h"

#include <cstddef>
#include <string>
#include <utility>

namespace Platform
{

/// Linux IProcessMemoryMapReader (#3089): /proc/[pid]/smaps (Smaps::parse()), read through a handle on
/// /proc/[pid] after its start time is checked against the target's, so a reused PID reads as exited rather
/// than showing a stranger's map.
///
/// The kernel serves smaps to the process's own user, or with ptrace read access (root, or CAP_SYS_PTRACE:
/// tasksmack-helper, which reads it for another user's process through this same reader); otherwise the
/// open fails with EACCES and the result is PermissionDenied. It reads only that fixed file of the
/// process's own directory, never a path a request names.
class LinuxProcessMemoryMapReader final : public IProcessMemoryMapReader
{
  public:
    /// How much of /proc/[pid]/smaps is read. The process being read decides the file's size: about 1 KiB
    /// per mapping and up to vm.max_map_count mappings (65530 by default), so tens of MB at most, read again
    /// on every refresh. A longer file is read this far, its last (possibly partial) mapping dropped, and the
    /// result marked truncated.
    static constexpr std::size_t MAX_SMAPS_BYTES = std::size_t{32} << 20U;

    LinuxProcessMemoryMapReader() = default;

    /// Test seam: read "<procRoot>/<pid>/..." instead of "/proc/<pid>/...", and at most @p maxSmapsBytes of
    /// smaps.
    explicit LinuxProcessMemoryMapReader(std::string procRoot, std::size_t maxSmapsBytes = MAX_SMAPS_BYTES)
        : m_ProcRoot(std::move(procRoot)), m_MaxSmapsBytes(maxSmapsBytes)
    {}

    [[nodiscard]] bool hasMemoryMap() const override;
    [[nodiscard]] MemoryMapReadResult readMemoryMap(const ProcessTarget& target) override;

  private:
    std::string m_ProcRoot = "/proc";
    std::size_t m_MaxSmapsBytes = MAX_SMAPS_BYTES;
};

} // namespace Platform
