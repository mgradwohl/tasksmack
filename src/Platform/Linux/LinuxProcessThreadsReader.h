#pragma once

#include "Platform/IProcessActions.h"
#include "Platform/IProcessThreads.h"

#include <cstddef>
#include <string>
#include <utility>

namespace Platform
{

/// Linux IProcessThreadsReader (#3069): each entry of /proc/[pid]/task, its stat line (state, CPU time,
/// nice, start time; ProcParsing::parseStatFields()), read through a handle on /proc/[pid] after its
/// start time is checked against the target's, so a reused PID reads as exited rather than listing a
/// stranger's threads. The start time is checked again after the listing, so a process that exits
/// meanwhile isn't shown with the threads it had.
///
/// A process's task directory is readable by anyone unless /proc is mounted with hidepid, where another
/// user's process is PermissionDenied. A thread that exits between the listing and its stat read is
/// left out.
class LinuxProcessThreadsReader final : public IProcessThreadsReader
{
  public:
    /// At most this many threads are read: a runaway process can have tens of thousands, and the
    /// section would read each one's stat every refresh.
    static constexpr std::size_t MAX_THREADS = 4096;

    LinuxProcessThreadsReader() = default;

    /// Test seam: read "<procRoot>/<pid>/..." instead of "/proc/<pid>/...".
    explicit LinuxProcessThreadsReader(std::string procRoot) : m_ProcRoot(std::move(procRoot))
    {}

    [[nodiscard]] bool hasThreads() const override;
    [[nodiscard]] ThreadsReadResult readThreads(const ProcessTarget& target) override;

  private:
    std::string m_ProcRoot = "/proc";
};

} // namespace Platform
