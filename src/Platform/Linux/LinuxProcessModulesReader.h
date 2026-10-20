#pragma once

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"

#include <cstddef>
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
    /// How much of /proc/[pid]/maps is read (#2642). The process being read decides the file's size: up
    /// to vm.max_map_count lines (65530 by default, 1048576 on some distributions) of up to PATH_MAX
    /// each, so hundreds of MB to GB, read again on every refresh. A real module list is under 1 MiB.
    /// A longer file is read this far, its last partial line dropped, and the result marked truncated.
    static constexpr std::size_t MAX_MAPS_BYTES = std::size_t{16} << 20U;

    LinuxProcessModulesReader() = default;

    /// Test seam: read "<procRoot>/<pid>/..." instead of "/proc/<pid>/...", and at most @p maxMapsBytes
    /// of maps.
    explicit LinuxProcessModulesReader(std::string procRoot, std::size_t maxMapsBytes = MAX_MAPS_BYTES)
        : m_ProcRoot(std::move(procRoot)), m_MaxMapsBytes(maxMapsBytes)
    {}

    [[nodiscard]] bool hasModules() const override;
    [[nodiscard]] ModulesReadResult readModules(const ProcessTarget& target) override;

  private:
    std::string m_ProcRoot = "/proc";
    std::size_t m_MaxMapsBytes = MAX_MAPS_BYTES;
};

} // namespace Platform
